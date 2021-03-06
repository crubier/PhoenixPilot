/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Sensors
 * @brief Acquires sensor data 
 * Specifically updates the the @ref Gyros, @ref Accels, and @ref Magnetometer objects
 * @{
 *
 * @file       sensors.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref Gyros @ref Accels @ref Magnetometer
 *
 * The module executes in its own thread.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "pios.h"
#include "homelocation.h"
#include "magnetometer.h"
#include "magbias.h"
#include "accels.h"
#include "gyros.h"
#include "gyrosbias.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "inertialsensorsettings.h"
#include "inssettings.h"
#include "revocalibration.h"
#include "flightstatus.h"
#include "CoordinateConversions.h"

#include <pios_board_info.h>

// Private constants
#define STACK_SIZE_BYTES 1000
#define TASK_PRIORITY (tskIDLE_PRIORITY+3)
#define SENSOR_PERIOD 2

#define F_PI 3.14159265358979323846f
#define PI_MOD(x) (fmodf(x + F_PI, F_PI * 2) - F_PI)
// Private types


// Private functions
static void SensorsTask(void *parameters);
static void settingsUpdatedCb(UAVObjEvent * objEv);
static void magOffsetEstimation(MagnetometerData *mag);

// Private variables
static xTaskHandle sensorsTaskHandle;
RevoCalibrationData revoCal;
INSSettingsData insSettings;

// These values are initialized by settings but can be updated by the attitude algorithm
static bool bias_correct_gyro = true;

static float mag_bias[3] = {0,0,0};
static float mag_scale[3] = {0,0,0};
static float accel_bias[3] = {0,0,0};
static float accel_scale[3] = {0,0,0};
static float gyro_scale[3] = {0,0,0};

static float Rbs[3][3] = {{0}};
static int8_t rotate = 0;

/**
 * API for sensor fusion algorithms:
 * Configure(xQueueHandle gyro, xQueueHandle accel, xQueueHandle mag, xQueueHandle baro)
 *   Stores all the queues the algorithm will pull data from
 * FinalizeSensors() -- before saving the sensors modifies them based on internal state (gyro bias)
 * Update() -- queries queues and updates the attitude estiamte
 */


/**
 * Initialise the module.  Called before the start function
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SensorsInitialize(void)
{
	GyrosInitialize();
	GyrosBiasInitialize();
	AccelsInitialize();
	MagnetometerInitialize();
	MagBiasInitialize();
	RevoCalibrationInitialize();
	AttitudeSettingsInitialize();
	InertialSensorSettingsInitialize();
	INSSettingsInitialize();

	rotate = 0;

	RevoCalibrationConnectCallback(&settingsUpdatedCb);
	AttitudeSettingsConnectCallback(&settingsUpdatedCb);
	InertialSensorSettingsConnectCallback(&settingsUpdatedCb);
	INSSettingsConnectCallback(&settingsUpdatedCb);

	return 0;
}

/**
 * Start the task.  Expects all objects to be initialized by this point.
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SensorsStart(void)
{
	// Start main task
	xTaskCreate(SensorsTask, (signed char *)"Sensors", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &sensorsTaskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_SENSORS, sensorsTaskHandle);
	PIOS_WDG_RegisterFlag(PIOS_WDG_SENSORS);

	return 0;
}

MODULE_INITCALL(SensorsInitialize, SensorsStart)

int32_t accel_test;
int32_t gyro_test;
int32_t mag_test;
//int32_t pressure_test;


/**
 * The sensor task.  This polls the gyros at 500 Hz and pumps that data to
 * stabilization and to the attitude loop
 * 
 * This function has a lot of if/defs right now to allow these configurations:
 * 1. BMA180 accel and MPU6000 gyro
 * 2. MPU6000 gyro and accel
 * 3. BMA180 accel and L3GD20 gyro
 */

uint32_t sensor_dt_us;
static void SensorsTask(void *parameters)
{
	portTickType lastSysTime;
	uint32_t accel_samples = 0;
	uint32_t gyro_samples = 0;
	int32_t accel_accum[3] = {0, 0, 0};
	int32_t gyro_accum[3] = {0,0,0};
	float gyro_scaling = 0;
	float accel_scaling = 0;
	static int32_t timeval;

	AlarmsClear(SYSTEMALARMS_ALARM_SENSORS);

	UAVObjEvent ev;
	settingsUpdatedCb(&ev);

	const struct pios_board_info * bdinfo = &pios_board_info_blob;	

	switch(bdinfo->board_rev) {
		case 0x01:
#if defined(PIOS_INCLUDE_L3GD20)
			gyro_test = PIOS_L3GD20_Test();
#endif
#if defined(PIOS_INCLUDE_BMA180)
			accel_test = PIOS_BMA180_Test();
#endif
			break;
		case 0x02:
#if defined(PIOS_INCLUDE_MPU6000)
			gyro_test = PIOS_MPU6000_Test();
			accel_test = gyro_test;
#endif
			break;
		default:
			PIOS_DEBUG_Assert(0);
	}

#if defined(PIOS_INCLUDE_HMC5883)
	mag_test = PIOS_HMC5883_Test();
#else
	mag_test = 0;
#endif

	if(accel_test < 0 || gyro_test < 0 || mag_test < 0) {
		AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_CRITICAL);
		while(1) {
			PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);
			vTaskDelay(10);
		}
	}
	
	// Main task loop
	lastSysTime = xTaskGetTickCount();
	bool error = false;
	uint32_t mag_update_time = PIOS_DELAY_GetRaw();
	while (1) {
		// TODO: add timeouts to the sensor reads and set an error if the fail
		sensor_dt_us = PIOS_DELAY_DiffuS(timeval);
		timeval = PIOS_DELAY_GetRaw();

		if (error) {
			PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);
			lastSysTime = xTaskGetTickCount();
			vTaskDelayUntil(&lastSysTime, SENSOR_PERIOD / portTICK_RATE_MS);
			AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_CRITICAL);
			error = false;
		} else {
			AlarmsClear(SYSTEMALARMS_ALARM_SENSORS);
		}


		for (int i = 0; i < 3; i++) {
			accel_accum[i] = 0;
			gyro_accum[i] = 0;
		}
		accel_samples = 0;
		gyro_samples = 0;

		AccelsData accelsData;
		GyrosData gyrosData;

		switch(bdinfo->board_rev) {
			case 0x01:  // L3GD20 + BMA180 board
#if defined(PIOS_INCLUDE_BMA180)
			{
				struct pios_bma180_data accel;
				
				int32_t read_good;
				int32_t count;
				
				count = 0;
				while((read_good = PIOS_BMA180_ReadFifo(&accel)) != 0 && !error)
					error = ((xTaskGetTickCount() - lastSysTime) > SENSOR_PERIOD) ? true : error;
				if (error) {
					// Unfortunately if the BMA180 ever misses getting read, then it will not
					// trigger more interrupts.  In this case we must force a read to kickstarts
					// it.
					struct pios_bma180_data data;
					PIOS_BMA180_ReadAccels(&data);
					continue;
				}
				while(read_good == 0) {	
					count++;
					
					accel_accum[1] += accel.x;
					accel_accum[0] += accel.y;
					accel_accum[2] -= accel.z;
					
					read_good = PIOS_BMA180_ReadFifo(&accel);
				}
				accel_samples = count;
				accel_scaling = PIOS_BMA180_GetScale();
				
				// Get temp from last reading
				accelsData.temperature = 25.0f + ((float) accel.temperature - 2.0f) / 2.0f;
			}
#endif
#if defined(PIOS_INCLUDE_L3GD20)
			{
				struct pios_l3gd20_data gyro;
				gyro_samples = 0;
				xQueueHandle gyro_queue = PIOS_L3GD20_GetQueue();
				
				if(xQueueReceive(gyro_queue, (void *) &gyro, 4) == errQUEUE_EMPTY) {
					error = true;
					continue;
				}
				
				gyro_samples = 1;
				gyro_accum[1] += gyro.gyro_x;
				gyro_accum[0] += gyro.gyro_y;
				gyro_accum[2] -= gyro.gyro_z;
				
				gyro_scaling = PIOS_L3GD20_GetScale();

				// Get temp from last reading
				gyrosData.temperature = gyro.temperature;
			}
#endif
				break;
			case 0x02:  // MPU6000 board
			case 0x03:  // MPU6000 board
#if defined(PIOS_INCLUDE_MPU6000)
			{
				struct pios_mpu6000_data mpu6000_data;
				xQueueHandle queue = PIOS_MPU6000_GetQueue();
				
				while(xQueueReceive(queue, (void *) &mpu6000_data, gyro_samples == 0 ? 10 : 0) != errQUEUE_EMPTY)
				{
					gyro_accum[0] += mpu6000_data.gyro_x;
					gyro_accum[1] += mpu6000_data.gyro_y;
					gyro_accum[2] += mpu6000_data.gyro_z;

					accel_accum[0] += mpu6000_data.accel_x;
					accel_accum[1] += mpu6000_data.accel_y;
					accel_accum[2] += mpu6000_data.accel_z;

					gyro_samples ++;
					accel_samples ++;
				}
				
				if (gyro_samples == 0) {
					PIOS_MPU6000_ReadGyros(&mpu6000_data);
					error = true;
					continue;
				}

				gyro_scaling = PIOS_MPU6000_GetScale();
				accel_scaling = PIOS_MPU6000_GetAccelScale();

				gyrosData.temperature = 35.0f + ((float) mpu6000_data.temperature + 512.0f) / 340.0f;
				accelsData.temperature = 35.0f + ((float) mpu6000_data.temperature + 512.0f) / 340.0f;
			}
#endif /* PIOS_INCLUDE_MPU6000 */
				break;
			default:
				PIOS_DEBUG_Assert(0);
		}

		// Average and scale the accels before rotation
		float accels[3] = {(float) accel_accum[0] / accel_samples, 
		                   (float) accel_accum[1] / accel_samples,
		                   (float) accel_accum[2] / accel_samples};
		float accels_out[3] = {accels[0] * accel_scaling * accel_scale[0] - accel_bias[0],
		                       accels[1] * accel_scaling * accel_scale[1] - accel_bias[1],
		                       accels[2] * accel_scaling * accel_scale[2] - accel_bias[2]};
		if (rotate) {
			rot_mult(Rbs, accels_out, accels, false);
			accelsData.x = accels[0];
			accelsData.y = accels[1];
			accelsData.z = accels[2];
		} else {
			accelsData.x = accels_out[0];
			accelsData.y = accels_out[1];
			accelsData.z = accels_out[2];
		}
		AccelsSet(&accelsData);

		// Scale the gyros
		float gyros[3] = {(float) gyro_accum[0] / gyro_samples,
		                  (float) gyro_accum[1] / gyro_samples,
		                  (float) gyro_accum[2] / gyro_samples};
		float gyros_out[3] = {gyros[0] * gyro_scaling * gyro_scale[0],
		                      gyros[1] * gyro_scaling * gyro_scale[1],
		                      gyros[2] * gyro_scaling * gyro_scale[2]};
		if (rotate) {
			rot_mult(Rbs, gyros_out, gyros, false);
			gyrosData.x = gyros[0];
			gyrosData.y = gyros[1];
			gyrosData.z = gyros[2];
		} else {
			gyrosData.x = gyros_out[0];
			gyrosData.y = gyros_out[1];
			gyrosData.z = gyros_out[2];
		}
		
		if (bias_correct_gyro) {
			// Apply bias correction to the gyros from the state estimator
			GyrosBiasData gyrosBias;
			GyrosBiasGet(&gyrosBias);
			gyrosData.x -= gyrosBias.x;
			gyrosData.y -= gyrosBias.y;
			gyrosData.z -= gyrosBias.z;
		}
		GyrosSet(&gyrosData);
		
		// Because most crafts wont get enough information from gravity to zero yaw gyro, we try
		// and make it average zero (weakly)

#if defined(PIOS_INCLUDE_HMC5883)
		MagnetometerData mag;
		if (PIOS_HMC5883_NewDataAvailable() || PIOS_DELAY_DiffuS(mag_update_time) > 150000) {
			struct pios_hmc5883_data mag_data;
			PIOS_HMC5883_ReadMag(&mag_data);
			float mags[3] = {(float) mag_data.mag_x * mag_scale[0] - mag_bias[0],
			                (float) mag_data.mag_y * mag_scale[1] - mag_bias[1],
			                (float) mag_data.mag_z * mag_scale[2] - mag_bias[2]};
			if (rotate) {
				float mag_out[3];
				rot_mult(Rbs, mags, mag_out, false);
				mag.x = mag_out[0];
				mag.y = mag_out[1];
				mag.z = mag_out[2];
			} else {
				mag.x = mags[0];
				mag.y = mags[1];
				mag.z = mags[2];
			}
			
			// Correct for mag bias and update if the rate is non zero
			if(insSettings.MagBiasNullingRate > 0)
				magOffsetEstimation(&mag);

			MagnetometerSet(&mag);
			mag_update_time = PIOS_DELAY_GetRaw();
		}
#endif

		PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);

		lastSysTime = xTaskGetTickCount();
	}
}

/**
 * Perform an update of the @ref MagBias based on
 * Magnetometer Offset Cancellation: Theory and Implementation, 
 * revisited William Premerlani, October 14, 2011
 */
static void magOffsetEstimation(MagnetometerData *mag)
{
#if 0
	// Constants, to possibly go into a UAVO
	static const float MIN_NORM_DIFFERENCE = 50;

	static float B2[3] = {0, 0, 0};

	MagBiasData magBias;
	MagBiasGet(&magBias);

	// Remove the current estimate of the bias
	mag->x -= magBias.x;
	mag->y -= magBias.y;
	mag->z -= magBias.z;

	// First call
	if (B2[0] == 0 && B2[1] == 0 && B2[2] == 0) {
		B2[0] = mag->x;
		B2[1] = mag->y;
		B2[2] = mag->z;
		return;
	}

	float B1[3] = {mag->x, mag->y, mag->z};
	float norm_diff = sqrtf(powf(B2[0] - B1[0],2) + powf(B2[1] - B1[1],2) + powf(B2[2] - B1[2],2));
	if (norm_diff > MIN_NORM_DIFFERENCE) {
		float norm_b1 = sqrtf(B1[0]*B1[0] + B1[1]*B1[1] + B1[2]*B1[2]);
		float norm_b2 = sqrtf(B2[0]*B2[0] + B2[1]*B2[1] + B2[2]*B2[2]);
		float scale = insSettings.MagBiasNullingRate * (norm_b2 - norm_b1) / norm_diff;
		float b_error[3] = {(B2[0] - B1[0]) * scale, (B2[1] - B1[1]) * scale, (B2[2] - B1[2]) * scale};

		magBias.x += b_error[0];
		magBias.y += b_error[1];
		magBias.z += b_error[2];

		MagBiasSet(&magBias);

		// Store this value to compare against next update
		B2[0] = B1[0]; B2[1] = B1[1]; B2[2] = B1[2];
	}
#else
	MagBiasData magBias;
	MagBiasGet(&magBias);
	
	// Remove the current estimate of the bias
	mag->x -= magBias.x;
	mag->y -= magBias.y;
	mag->z -= magBias.z;
	
	HomeLocationData homeLocation;
	HomeLocationGet(&homeLocation);
	
	AttitudeActualData attitude;
	AttitudeActualGet(&attitude);
	
	const float Rxy = sqrtf(homeLocation.Be[0]*homeLocation.Be[0] + homeLocation.Be[1]*homeLocation.Be[1]);
	const float Rz = homeLocation.Be[2];
	
	const float rate = insSettings.MagBiasNullingRate;
	float R[3][3];
	float B_e[3];
	float xy[2];
	float delta[3];
	
	// Get the rotation matrix
	Quaternion2R(&attitude.q1, R);
	
	// Rotate the mag into the NED frame
	B_e[0] = R[0][0] * mag->x + R[1][0] * mag->y + R[2][0] * mag->z;
	B_e[1] = R[0][1] * mag->x + R[1][1] * mag->y + R[2][1] * mag->z;
	B_e[2] = R[0][2] * mag->x + R[1][2] * mag->y + R[2][2] * mag->z;
	
	float cy = cosf(attitude.Yaw * M_PI / 180.0f);
	float sy = sinf(attitude.Yaw * M_PI / 180.0f);
	
	xy[0] =  cy * B_e[0] + sy * B_e[1];
	xy[1] = -sy * B_e[0] + cy * B_e[1];
	
	float xy_norm = sqrtf(xy[0]*xy[0] + xy[1]*xy[1]);
	
	delta[0] = -rate * (xy[0] / xy_norm * Rxy - xy[0]);
	delta[1] = -rate * (xy[1] / xy_norm * Rxy - xy[1]);
	delta[2] = -rate * (Rz - B_e[2]);
	
	if (delta[0] == delta[0] && delta[1] == delta[1] && delta[2] == delta[2]) {		
		magBias.x += delta[0];
		magBias.y += delta[1];
		magBias.z += delta[2];
		MagBiasSet(&magBias);
	}
#endif
}

/**
 * Locally cache some variables from the AtttitudeSettings object
 */
static void settingsUpdatedCb(UAVObjEvent * objEv) {
	RevoCalibrationGet(&revoCal);
	InertialSensorSettingsData inertialSensorSettings;
	InertialSensorSettingsGet(&inertialSensorSettings);
	INSSettingsGet(&insSettings);
	
	mag_bias[0] = revoCal.MagBias[REVOCALIBRATION_MAGBIAS_X];
	mag_bias[1] = revoCal.MagBias[REVOCALIBRATION_MAGBIAS_Y];
	mag_bias[2] = revoCal.MagBias[REVOCALIBRATION_MAGBIAS_Z];
	mag_scale[0] = revoCal.MagScale[REVOCALIBRATION_MAGSCALE_X];
	mag_scale[1] = revoCal.MagScale[REVOCALIBRATION_MAGSCALE_Y];
	mag_scale[2] = revoCal.MagScale[REVOCALIBRATION_MAGSCALE_Z];
	accel_bias[0] = inertialSensorSettings.AccelBias[INERTIALSENSORSETTINGS_ACCELBIAS_X];
	accel_bias[1] = inertialSensorSettings.AccelBias[INERTIALSENSORSETTINGS_ACCELBIAS_Y];
	accel_bias[2] = inertialSensorSettings.AccelBias[INERTIALSENSORSETTINGS_ACCELBIAS_Z];
	accel_scale[0] = inertialSensorSettings.AccelScale[INERTIALSENSORSETTINGS_ACCELSCALE_X];
	accel_scale[1] = inertialSensorSettings.AccelScale[INERTIALSENSORSETTINGS_ACCELSCALE_X];
	accel_scale[2] = inertialSensorSettings.AccelScale[INERTIALSENSORSETTINGS_ACCELSCALE_X];
	gyro_scale[0] = inertialSensorSettings.GyroScale[INERTIALSENSORSETTINGS_GYROSCALE_X];
	gyro_scale[1] = inertialSensorSettings.GyroScale[INERTIALSENSORSETTINGS_GYROSCALE_X];
	gyro_scale[2] = inertialSensorSettings.GyroScale[INERTIALSENSORSETTINGS_GYROSCALE_X];
	
	// Zero out any adaptive tracking
	MagBiasData magBias;
	MagBiasGet(&magBias);
	magBias.x = 0;
	magBias.y = 0;
	magBias.z = 0;
	MagBiasSet(&magBias);

	uint8_t bias_correct;
	AttitudeSettingsBiasCorrectGyroGet(&bias_correct);
	bias_correct_gyro = (bias_correct == ATTITUDESETTINGS_BIASCORRECTGYRO_TRUE);

	AttitudeSettingsData attitudeSettings;
	AttitudeSettingsGet(&attitudeSettings);
	// Indicates not to expend cycles on rotation
	if(attitudeSettings.BoardRotation[0] == 0 && attitudeSettings.BoardRotation[1] == 0 &&
	   attitudeSettings.BoardRotation[2] == 0) {
		rotate = 0;
	} else {
		float rotationQuat[4];
		const float rpy[3] = {attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_ROLL] / 100.0f,
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_PITCH] / 100.0f,
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_YAW] / 100.0f};
		RPY2Quaternion(rpy, rotationQuat);
		Quaternion2R(rotationQuat, Rbs);
		rotate = 1;
	}

}
/**
  * @}
  * @}
  */
