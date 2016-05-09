#include "stepperController.h"
#include "serial.h"


int32_t GetStepDirectionUnit(stepper_state * stepper){
    return (stepper->status & SS_RUNNING_BACKWARD) ? -1 : 1;
}

int32_t GetStepsToTarget(stepper_state * stepper) {
    // returns absolute value of steps left to target
    return GetStepDirectionUnit(stepper) * (stepper->targetPosition - stepper->currentPosition);
}

void UpdateStepTimerToCurrentSPS(stepper_state * stepper){
    TIM_TypeDef * timer = stepper -> STEP_TIMER -> Instance;
    uint32_t prescaler = 0;
    uint32_t timerTicks = STEP_TIMER_CLOCK / stepper -> currentSPS;
    
    if (timerTicks > 0xFFFF) {
        // calculate the minimum prescaler
        prescaler = timerTicks/0xFFFF;
        timerTicks /= (prescaler + 1);
    }
    
    timer -> PSC = prescaler;
    timer -> ARR = timerTicks;    
}

void DecrementSPS(stepper_state * stepper){
    if (stepper -> currentSPS > stepper -> minSPS){
        stepper -> currentSPS -=  stepper -> accelerationSPS;
        UpdateStepTimerToCurrentSPS(stepper);
    }
}

void IncrementSPS(stepper_state * stepper){
    if (stepper -> currentSPS < stepper -> maxSPS) {
        stepper -> currentSPS +=  stepper -> accelerationSPS;
        UpdateStepTimerToCurrentSPS(stepper);
    }
}

void StepHandler(stepper_state * stepper){
  switch (stepper->status & ~(SS_BREAKING|SS_BREAKCORRECTION)){
    case SS_STARTING:
        if (stepper->currentPosition > stepper->targetPosition){
            stepper->status = SS_RUNNING_BACKWARD;
            stepper->DIR_GPIO->BSRR = (uint32_t)stepper->DIR_PIN << 16u;
        } else if (stepper->currentPosition < stepper->targetPosition){
            stepper->status = SS_RUNNING_FORWARD;
            stepper->DIR_GPIO->BSRR = stepper->DIR_PIN;
        }     
      break;   
    case SS_RUNNING_FORWARD:
    case SS_RUNNING_BACKWARD: {
        int32_t stepsToTarget;
        // The actual pulse has been generated by previous timer run.
        stepper->currentPosition += GetStepDirectionUnit(stepper);
        stepsToTarget = GetStepsToTarget(stepper) ;

        if (stepsToTarget < 0 || (stepsToTarget > 0 && stepper->currentSPS == stepper->minSPS)) {
          // TODO: SEND BREAKING UNDER/OVER-ESTIMATION ERROR REPORT
        }
        if (stepsToTarget <= 0 && stepper -> currentSPS == stepper -> minSPS) {
            // We reached or passed through our target position at the stopping speed
            stepper->status = SS_STOPPED;
            HAL_TIM_PWM_Stop(stepper->STEP_TIMER, stepper->STEP_CHANNEL);
            Serial_WriteString(stepper->name);
            Serial_WriteString(".stop:");
            Serial_WriteInt(stepper->currentPosition);
            Serial_WriteString("\r\n");
        }}
      break;
  }
}

void StepControllerHandler(stepper_state * stepper){
    stepper_status status = stepper -> status;
  
    if (status & SS_STOPPED) { 
       if (stepper->targetPosition != stepper->currentPosition) {
         stepper->stepCtrlPrescallerTicks = stepper->stepCtrlPrescaller;
         stepper->status = SS_STARTING;
         stepper->STEP_TIMER->Instance->EGR = TIM_EGR_UG;
         HAL_TIM_PWM_Start(stepper->STEP_TIMER, stepper->STEP_CHANNEL);
       }
       return;
    }
    
    if (status == SS_STARTING)
      return;

    stepper->stepCtrlPrescallerTicks--;
    
    if (!(status & SS_BREAKING))
    {
        // check - do we need to break
        // to do that - we take average speed (between current and minimum)
        // and using it to calculate how much time left to the stopping point 
     
        // Steps to target deevided by average speed.
        float estimatedTimeToTarget = 2.0f * GetStepsToTarget(stepper) / (stepper->currentSPS + stepper->minSPS);
        uint32_t spsSwitches        = (stepper->currentSPS - stepper->minSPS) / stepper->accelerationSPS;
        float timeToReduceSpeed     =
            (((float)STEP_CONTROLLER_PERIOD_US)/ 1000000.0f) *
            ((uint64_t)(stepper->stepCtrlPrescaller) * spsSwitches + stepper->stepCtrlPrescallerTicks);

        // If we are in condition to start bracking, but not breaking yet
        if (estimatedTimeToTarget <= timeToReduceSpeed) {
            
            stepper->breakInitiationSPS = stepper->currentSPS;
            stepper->status &= ~SS_BREAKCORRECTION;
            stepper->status |= SS_BREAKING;
            
            DecrementSPS(stepper);

            // So we terminated onging acceleration, or immidiately switched back from top speed        
            if (stepper->stepCtrlPrescallerTicks == 0)
                stepper->stepCtrlPrescallerTicks = stepper->stepCtrlPrescaller;
            
            // we are done with this check 
            return;
        }
    }

    if (stepper->stepCtrlPrescallerTicks == 0) {

        if (status & SS_BREAKING) {
            // check, mabe we don't need to break any more, because earlier overestimation
            int32_t spsSwitchesOnBreakeInitiated = (stepper->breakInitiationSPS - stepper->minSPS) / stepper->accelerationSPS;
            int32_t spsSwitchesLeft              = (stepper->currentSPS         - stepper->minSPS) / stepper->accelerationSPS;

              
            // if we already reduced our speed twice
            // and we still at sufficient speed 
            // re-evaluete "do we still need breaking, or can relax and roll for a while?"
            if (spsSwitchesOnBreakeInitiated/2 > spsSwitchesLeft && spsSwitchesLeft > 10) {
               stepper->status |= SS_BREAKCORRECTION;
               stepper->status &= ~SS_BREAKING;
            }

            // we still have to execute breaking transition here
            DecrementSPS(stepper);
        }
        else if (!(status & SS_BREAKCORRECTION)){
            IncrementSPS(stepper);
        }
        
        stepper->stepCtrlPrescallerTicks = stepper->stepCtrlPrescaller;
    }
}

void InitStepperState(char * name, stepper_state * stepper, TIM_HandleTypeDef * stepTimer, uint32_t stepChannel, GPIO_TypeDef  * dirGPIO, uint16_t dirPIN) {
    // ensure that ARR preload mode is enabled on timer
    // but we don't need to ste the PWM pulse duration preload, it is constant all the time
    stepTimer -> Instance -> CR1 |=TIM_CR1_ARPE;
    
    stepper -> name             = name;
    stepper -> STEP_TIMER       = stepTimer;
    stepper -> STEP_CHANNEL     = stepChannel;
    stepper -> DIR_GPIO         = dirGPIO;
    stepper -> DIR_PIN          = dirPIN;
    
    stepper -> minSPS           = 150;       // this is like an hour per turn in microstepping mode
    stepper -> maxSPS           = 400000;   // 400kHz is 2.5uS per step, while theoretically possible limit for dirver is 2uS (as per A4988 datasheet)
    stepper -> currentSPS       = stepper -> minSPS;
    stepper -> accelerationSPS  = stepper -> minSPS;
    
    // minimum starting sppeed depends on inertia
    // so take how many microseconds we have per step at minimum speed,
    // and divide it by the minimum controller period
    stepper -> stepCtrlPrescaller      = 1 + ((1000000u / (stepper -> minSPS+stepper -> accelerationSPS+1)) / STEP_CONTROLLER_PERIOD_US);
    stepper -> stepCtrlPrescallerTicks = stepper -> stepCtrlPrescaller;
    
    
    // zero service fields
    stepper -> targetPosition           = 0;
    stepper -> currentPosition          = 0;
    stepper -> breakInitiationSPS       = stepper -> maxSPS;
   
    stepper -> status = SS_STOPPED;
}

