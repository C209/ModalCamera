// Copyright Chronicler.

#include "CameraModes/CameraMode_Fixed.h"

#include "CameraAssistInterface.h"
#include "ModalCameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Canvas.h"
#include "Engine/DebugCameraController.h"
#include "GameFramework/CameraBlockingVolume.h"
#include "GameFramework/Controller.h"
#include "Kismet/GameplayStatics.h"
#include "Math/RotationMatrix.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(CameraMode_Fixed)

namespace CameraMode_Fixed_Statics
{
	static const FName NAME_IgnoreCameraCollision = TEXT("IgnoreCameraCollision");
}

UCameraMode_Fixed::UCameraMode_Fixed()
	: AimLineToDesiredPosBlockedPct(0) { }

void UCameraMode_Fixed::OnActivation()
{
	TArray<AActor*> DebugCameras;
	UGameplayStatics::GetAllActorsOfClass(UModalCameraMode::GetWorld(), ADebugCameraController::StaticClass(), DebugCameras);
	if (DebugCameras.Num() > 0)
	{
		if (ADebugCameraController* DebugCameraController = Cast<ADebugCameraController>(DebugCameras.Last()))
		{
			DebugCameraController->GetPlayerViewPoint(FixedLocation, FixedRotation);
			// Fixed Camera always replaces the debug camera, so force clean-up for it now to prevent confused data
			// states later.
			DebugCameraController->Destroy();
			return;
		}
	}
	GetCameraComponent()->GetController<APlayerController>()->GetPlayerViewPoint(FixedLocation, FixedRotation);
}

void UCameraMode_Fixed::UpdateView(const float DeltaTime)
{
	const FVector PivotLocation = GetPivotLocation();
	FRotator PivotRotation = GetPivotRotation();

	PivotRotation.Pitch = FMath::ClampAngle(PivotRotation.Pitch, ViewPitchMin, ViewPitchMax);

	View.Location = PivotLocation;
	View.Rotation = PivotRotation;
	View.ControlRotation = View.Rotation;
	View.FieldOfView = FieldOfView;

	// Adjust final desired camera location to prevent any penetration
	UpdatePreventPenetration(DeltaTime);
}

void UCameraMode_Fixed::DrawDebug(UCanvas* Canvas) const
{
	Super::DrawDebug(Canvas);

#if ENABLE_DRAW_DEBUG
	FDisplayDebugManager& DisplayDebugManager = Canvas->DisplayDebugManager;
	for (int i = 0; i < DebugActorsHitDuringCameraPenetration.Num(); i++)
	{
		DisplayDebugManager.DrawString(
			FString::Printf(TEXT("HitActorDuringPenetration[%d]: %s")
				, i
				, *DebugActorsHitDuringCameraPenetration[i]->GetName()));
	}

	LastDrawDebugTime = GetWorld()->GetTimeSeconds();
#endif
}

FVector UCameraMode_Fixed::GetPivotLocation() const
{
	return FixedLocation;
}

FRotator UCameraMode_Fixed::GetPivotRotation() const
{
	return FixedRotation;
}

void UCameraMode_Fixed::UpdatePreventPenetration(float DeltaTime)
{
	if (!bPreventPenetration)
	{
		return;
	}

	AActor* TargetActor = GetTargetActor();

	const APawn* TargetPawn = Cast<APawn>(TargetActor);
	AController* TargetController = TargetPawn ? TargetPawn->GetController() : nullptr;
	ICameraAssistInterface* TargetControllerAssist = Cast<ICameraAssistInterface>(TargetController);

	ICameraAssistInterface* TargetActorAssist = Cast<ICameraAssistInterface>(TargetActor);

	TOptional<AActor*> OptionalPPTarget = TargetActorAssist ? TargetActorAssist->GetCameraPreventPenetrationTarget() : TOptional<AActor*>();
	AActor* PPActor = OptionalPPTarget.IsSet() ? OptionalPPTarget.GetValue() : TargetActor;
	ICameraAssistInterface* PPActorAssist = OptionalPPTarget.IsSet() ? Cast<ICameraAssistInterface>(PPActor) : nullptr;

	if (const UPrimitiveComponent* PPActorRootComponent = Cast<UPrimitiveComponent>(PPActor->GetRootComponent()))
	{
		// Attempt at picking SafeLocation automatically, so we reduce camera translation when aiming.
		// Our camera is our reticle, so we want to preserve our aim and keep that as steady and smooth as possible.
		// Pick the closest point on capsule to our aim line.
		FVector ClosestPointOnLineToCapsuleCenter;
		FVector SafeLocation = PPActor->GetActorLocation();
		FMath::PointDistToLine(SafeLocation, View.Rotation.Vector(), View.Location, ClosestPointOnLineToCapsuleCenter);

		// Adjust Safe distance height to be same as aim line, but within capsule.
		float const PushInDistance = CollisionPushOutDistance;
		float const MaxHalfHeight = PPActor->GetSimpleCollisionHalfHeight() - PushInDistance;
		SafeLocation.Z = FMath::Clamp(ClosestPointOnLineToCapsuleCenter.Z, SafeLocation.Z - MaxHalfHeight, SafeLocation.Z + MaxHalfHeight);

		float DistanceSqr;
		PPActorRootComponent->GetSquaredDistanceToCollision(ClosestPointOnLineToCapsuleCenter, DistanceSqr, SafeLocation);

		// Then aim line to desired camera position
		PreventCameraPenetration(*PPActor, SafeLocation, View.Location, DeltaTime, AimLineToDesiredPosBlockedPct, true);

		ICameraAssistInterface* AssistArray[] = { TargetControllerAssist, TargetActorAssist, PPActorAssist };

		if (AimLineToDesiredPosBlockedPct < ReportPenetrationPercent)
		{
			for (ICameraAssistInterface* Assist : AssistArray)
			{
				if (Assist)
				{
					// camera is too close, tell the assists
					Assist->OnCameraPenetratingTarget();
				}
			}
		}
	}
}

void UCameraMode_Fixed::PreventCameraPenetration(class AActor const& ViewTarget, FVector const& SafeLoc, FVector& CameraLoc, float const& DeltaTime, float& DistBlockedPct, bool bSingleRayOnly)
{
#if ENABLE_DRAW_DEBUG
	DebugActorsHitDuringCameraPenetration.Reset();
#endif

	float HardBlockedPct = DistBlockedPct;
	float SoftBlockedPct = DistBlockedPct;

	FVector BaseRay = CameraLoc - SafeLoc;
	FRotationMatrix BaseRayMatrix(BaseRay.Rotation());
	FVector BaseRayLocalUp, BaseRayLocalFwd, BaseRayLocalRight;

	BaseRayMatrix.GetScaledAxes(BaseRayLocalFwd, BaseRayLocalRight, BaseRayLocalUp);

	float DistBlockedPctThisFrame = 1.f;

	int32 const NumRaysToShoot = bSingleRayOnly ? 1 : 4;
	FCollisionQueryParams SphereParams(SCENE_QUERY_STAT(CameraPen), false, nullptr/*PlayerCamera*/);

	SphereParams.AddIgnoredActor(&ViewTarget);
	FCollisionShape SphereShape = FCollisionShape::MakeSphere(0.f);
	UWorld* World = GetWorld();

	for (int32 RayIdx = 0; RayIdx < NumRaysToShoot; ++RayIdx)
	{
		// calc ray target
		FVector RayTarget;
		{
			RayTarget = SafeLoc;
		}

		ECollisionChannel TraceChannel = ECC_Camera;		//(Feeler.PawnWeight > 0.f) ? ECC_Pawn : ECC_Camera;

		// do multi-line check to make sure the hits we throw out aren't
		// masking real hits behind (these are important rays).

		// MT-> passing camera as actor so that camerablockingvolumes know when it's the camera doing traces
		FHitResult Hit;
		const bool bHit = World->SweepSingleByChannel(Hit, SafeLoc, RayTarget, FQuat::Identity, TraceChannel, SphereShape, SphereParams);
#if ENABLE_DRAW_DEBUG
		if (World->TimeSince(LastDrawDebugTime) < 1.f)
		{
			DrawDebugSphere(World, SafeLoc, SphereShape.Sphere.Radius, 8, FColor::Red);
			DrawDebugSphere(World, bHit ? Hit.Location : RayTarget, SphereShape.Sphere.Radius, 8, FColor::Red);
			DrawDebugLine(World, SafeLoc, bHit ? Hit.Location : RayTarget, FColor::Red);
		}
#endif // ENABLE_DRAW_DEBUG


		if (const AActor* HitActor = Hit.GetActor(); bHit && HitActor)
		{
			bool bIgnoreHit = false;

			if (HitActor->ActorHasTag(CameraMode_Fixed_Statics::NAME_IgnoreCameraCollision))
			{
				bIgnoreHit = true;
				SphereParams.AddIgnoredActor(HitActor);
			}

			// Ignore CameraBlockingVolume hits that occur in front of the ViewTarget.
			if (!bIgnoreHit && HitActor->IsA<ACameraBlockingVolume>())
			{
				const FVector ViewTargetForwardXY = ViewTarget.GetActorForwardVector().GetSafeNormal2D();
				const FVector ViewTargetLocation = ViewTarget.GetActorLocation();
				const FVector HitOffset = Hit.Location - ViewTargetLocation;
				const FVector HitDirectionXY = HitOffset.GetSafeNormal2D();
				if (const float DotHitDirection = FVector::DotProduct(ViewTargetForwardXY, HitDirectionXY); DotHitDirection > 0.0f)
				{
					bIgnoreHit = true;
					// Ignore this CameraBlockingVolume on the remaining sweeps.
					SphereParams.AddIgnoredActor(HitActor);
				}
				else
				{
#if ENABLE_DRAW_DEBUG
					DebugActorsHitDuringCameraPenetration.AddUnique(TObjectPtr<const AActor>(HitActor));
#endif
				}
			}

			if (!bIgnoreHit)
			{
				float NewBlockPct = Hit.Time;
				NewBlockPct += (1.f - NewBlockPct);

				// Recompute blocked pct taking into account pushout distance.
				NewBlockPct = ((Hit.Location - SafeLoc).Size() - CollisionPushOutDistance) / (RayTarget - SafeLoc).Size();
				DistBlockedPctThisFrame = FMath::Min(NewBlockPct, DistBlockedPctThisFrame);

#if ENABLE_DRAW_DEBUG
				DebugActorsHitDuringCameraPenetration.AddUnique(TObjectPtr<const AActor>(HitActor));
#endif
			}
		}

		if (RayIdx == 0)
		{
			// don't interpolate toward this one, snap to it
			// assumes ray 0 is the center/main ray
			HardBlockedPct = DistBlockedPctThisFrame;
		}
		else
		{
			SoftBlockedPct = DistBlockedPctThisFrame;
		}
	}

	if (bResetInterpolation)
	{
		DistBlockedPct = DistBlockedPctThisFrame;
	}
	else if (DistBlockedPct < DistBlockedPctThisFrame)
	{
			DistBlockedPct = DistBlockedPctThisFrame;
	}
	else
	{
		if (DistBlockedPct > HardBlockedPct)
		{
			DistBlockedPct = HardBlockedPct;
		}
		else if (DistBlockedPct > SoftBlockedPct)
		{
			DistBlockedPct = SoftBlockedPct;
		}
	}

	DistBlockedPct = FMath::Clamp<float>(DistBlockedPct, 0.f, 1.f);
	if (DistBlockedPct < (1.f - ZERO_ANIMWEIGHT_THRESH))
	{
		CameraLoc = SafeLoc + (CameraLoc - SafeLoc) * DistBlockedPct;
	}
}
