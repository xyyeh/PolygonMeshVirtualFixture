/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-    */
/* ex: set filetype=cpp softtabstop=4 shiftwidth=4 tabstop=4 cindent expandtab: */

/*
  Author(s):  Max Zhaoshuo Li
  Created on: 2020-01-24

  (C) Copyright 2020 Johns Hopkins University (JHU), All Rights Reserved.

--- begin cisst license - do not edit ---

This software is provided "as is" under an open source license, with
no warranty.  The complete license can be found in license.txt and
http://www.cisst.org/cisst/license.txt.

--- end cisst license ---
*/

#include <mtsDerivedTeleOperationPSM.h>

#include <cisstMultiTask/mtsInterfaceRequired.h>
#include <cisstParameterTypes/prmForceCartesianGet.h>
#include <cisstParameterTypes/prmVelocityCartesianGet.h>
#include <cisstParameterTypes/prmForceCartesianSet.h>

#include <cmath>

CMN_IMPLEMENT_SERVICES_DERIVED_ONEARG(mtsDerivedTeleOperationPSM,
                                      mtsTeleOperationPSM,
                                      mtsTaskPeriodicConstructorArg);

mtsDerivedTeleOperationPSM::mtsDerivedTeleOperationPSM(const std::string & componentName,
                                                       const double periodInSeconds):
    mtsTeleOperationPSM(componentName, periodInSeconds)
{
}

mtsDerivedTeleOperationPSM::mtsDerivedTeleOperationPSM(const mtsTaskPeriodicConstructorArg & arg):
    mtsTeleOperationPSM(arg)
{
}

// Configure is a virtual method, we can redefine it and have our own
// configuration
void mtsDerivedTeleOperationPSM::Configure(const std::string & CMN_UNUSED(filename))
{
    // Call the base class configure, it will do most of the work
    BaseType::Configure();

    // We want to replace the method called when the MTM is actually
    // driving the PSM
    mTeleopState.SetEnterCallback("ENABLED",
                                  &mtsDerivedTeleOperationPSM::EnterEnabled,
                                  this);
    mTeleopState.SetRunCallback("ENABLED",
                                &mtsDerivedTeleOperationPSM::RunEnabled,
                                this);

    // We need to add a method to retrieve estimated wrench from the
    // PSM
    mtsInterfaceRequired * interfacePSM = GetInterfaceRequired("PSM");
    // That interface should exist, abort otherwise
    CMN_ASSERT(interfacePSM);
    // Add a required function
    interfacePSM->AddFunction("body/measured_cf",
                              PSMGetWrenchBody);
    interfacePSM->AddFunction("SetWrenchBodyOrientationAbsolute",
                              PSMSetWrenchBodyOrientationAbsolute);
    interfacePSM->AddFunction("measured_cv",
                              PSMGetVelocityCartesian);
    interfacePSM->AddFunction("SetConstraintMotionEnable",
                              PSMSetConstraintMotionEnable);
    interfacePSM->AddFunction("GetSimulation",
                              PSMGetSimulation);

    // Same for MTM
    mtsInterfaceRequired * interfaceMTM = GetInterfaceRequired("MTM");
    CMN_ASSERT(interfaceMTM);
    interfaceMTM->AddFunction("SetWrenchBodyOrientationAbsolute",
                              MTMSetWrenchBodyOrientationAbsolute,
                              MTS_OPTIONAL);

    // ignore jaw
    mIgnoreJaw = true;

    // lock rotation
    mRotationLocked = true;

    // skip aligning MTM
    mAlignMTM = false;

    elasticityGain.Assign(1.0,1.0,1.0).Multiply(150.0);
}

void mtsDerivedTeleOperationPSM::EnterEnabled(void)
{
    BaseType::EnterEnabled();
    PSMSetWrenchBodyOrientationAbsolute(true);
    // function SetWrenchBodyOrientationAbsolute is optional on MTM, only call if available
    if (MTMSetWrenchBodyOrientationAbsolute.IsValid()) {
        MTMSetWrenchBodyOrientationAbsolute(true);
    }
}

void mtsDerivedTeleOperationPSM::RunEnabled(void)
{
    BaseType::RunEnabled();

    // Use proxy location to set haptics feedback; Only if the PSM is following
    if (mMTM.PositionCartesianCurrent.Valid()
           && mPSM.PositionCartesianCurrent.Valid())
    {
        if (!mIsClutched && mIsFollowing){
            // compute mtm Cartesian motion
            vctFrm4x4 mtmPosition(mMTM.PositionCartesianCurrent.Position());

            // translation
            vct3 mtmTranslation;
            vct3 psmTranslation;
            if (mTranslationLocked) {
                psmTranslation = mPSM.CartesianInitial.Translation();
            } else {
                mtmTranslation = (mtmPosition.Translation() - mMTM.CartesianInitial.Translation());
                psmTranslation = mtmTranslation * mScale;
                psmTranslation = mRegistrationRotation * psmTranslation + mPSM.CartesianInitial.Translation();
            }
            // rotation
            vctMatRot3 psmRotation;
            if (mRotationLocked) {
                psmRotation.From(mPSM.CartesianInitial.Rotation());
            } else {
                psmRotation = mRegistrationRotation * mtmPosition.Rotation() * mAlignOffsetInitial;
            }

            vctFrm4x4 psmCartesianGoal;
            psmCartesianGoal.Translation().Assign(psmTranslation);
            psmCartesianGoal.Rotation().FromNormalized(psmRotation);

            // proxy force
            prmPositionCartesianGet psmMeasuredCartesian;
            mPSM.measured_cp(psmMeasuredCartesian);
            vct3 diff = psmMeasuredCartesian.Position().Translation() - psmCartesianGoal.Translation();
            vct3 force;
            for (size_t i=0 ; i < 3; i++){
                force[i] = elasticityGain[i] * diff[i];
            }

            // Re-orient based on rotation between MTM and PSM
            force = mRegistrationRotation.Inverse() * force;
            // set force to MTM
            bool psmSimulated;
            PSMGetSimulation(psmSimulated);
            if (!psmSimulated){
                prmForceCartesianSet wrenchMTM;
                wrenchMTM.Force().Ref<3>(0) = force;
                mMTM.servo_cf_body(wrenchMTM);
            }
        }
    }
}

void mtsDerivedTeleOperationPSM::TransitionEnabled()
{
    std::cout << "Transition Enabled" << std::endl;

    BaseType::TransitionEnabled();

    // tell derived psm that constraint motion should be disabled
    PSMSetConstraintMotionEnable(false);
}
