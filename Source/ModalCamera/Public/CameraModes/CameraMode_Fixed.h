// Copyright Chronicler.

#pragma once

#include "DrawDebugHelpers.h"
#include "../ModalCameraMode.h"
#include "Curves/CurveFloat.h"

#include "CameraMode_Fixed.generated.h"

class UCurveVector;

/**
 * UCameraMode_Fixed
 *
 *	A basic Fixed camera mode.
 */
UCLASS(Abstract, Blueprintable)
class UCameraMode_Fixed : public UModalCameraMode
{
	GENERATED_BODY()

public:

	UCameraMode_Fixed();

protected:

	virtual void UpdateView(float DeltaTime) override;

	void UpdatePreventPenetration(float DeltaTime);
	void PreventCameraPenetration(class AActor const& ViewTarget, FVector const& SafeLoc, FVector& CameraLoc, float const& DeltaTime, float& DistBlockedPct, bool bSingleRayOnly);

	virtual void DrawDebug(UCanvas* Canvas) const override;

	virtual FVector GetPivotLocation() const override;

	virtual FRotator GetPivotRotation() const override;

	virtual void OnActivation() override;

protected:
	UPROPERTY(EditDefaultsOnly, Category = "Fixed")
	FVector FixedLocation;

	UPROPERTY(EditDefaultsOnly, Category = "Fixed")
	FRotator FixedRotation;

	/** If true, does collision checks to keep the camera out of the world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Collision")
	bool bPreventPenetration = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision")
	float CollisionPushOutDistance = 2.f;

	/** When the camera's distance is pushed into this percentage of its full distance due to penetration */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision")
	float ReportPenetrationPercent = 0.f;

	UPROPERTY(Transient)
	float AimLineToDesiredPosBlockedPct;

	UPROPERTY(Transient)
	TArray<TObjectPtr<const AActor>> DebugActorsHitDuringCameraPenetration;

#if ENABLE_DRAW_DEBUG
	mutable float LastDrawDebugTime = -MAX_FLT;
#endif

};
