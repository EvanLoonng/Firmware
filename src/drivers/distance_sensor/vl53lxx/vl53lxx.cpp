/****************************************************************************
 *
 *   Copyright (c) 2013-2017 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file vl53lxx.cpp
 * @author Daniele Pettenuzzo
 *
 * Driver for the vl53lxx ToF Sensor from ST Microelectronics connected via I2C.
 */

#include <px4_config.h>
#include <px4_defines.h>
#include <px4_getopt.h>
#include <px4_workqueue.h>

#include <drivers/device/i2c.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <semaphore.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

#include <systemlib/perf_counter.h>
#include <systemlib/err.h>

#include <drivers/drv_hrt.h>
#include <drivers/drv_range_finder.h>
#include <drivers/device/ringbuffer.h>

#include <uORB/uORB.h>
#include <uORB/topics/subsystem_info.h>
#include <uORB/topics/distance_sensor.h>

#include <board_config.h>

/* Configuration Constants */
#ifdef PX4_I2C_BUS_EXPANSION1
#define VL53LXX_BUS 			PX4_I2C_BUS_EXPANSION1		// I2C2 on fmu-v5
#else
#define VL53LXX_BUS 			PX4_I2C_BUS_EXPANSION 		// I2C on all others (pixracer)
#endif

#define VL53LXX_BASEADDR      	0b0101001 					/* 7-bit address */
#define VL53LXX_DEVICE_PATH   	"/dev/vl53lxx"

/* VL53LXX Registers addresses */
#define VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HW_REG				0x89
#define MSRC_CONFIG_CONTROL_REG								0x60
#define FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT_REG		0x44
#define SYSTEM_SEQUENCE_CONFIG_REG							0x01
#define DYNAMIC_SPAD_REF_EN_START_OFFSET_REG				0x4F
#define DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD_REG				0x4E
#define GLOBAL_CONFIG_REF_EN_START_SELECT_REG				0xB6
#define GLOBAL_CONFIG_SPAD_ENABLES_REF_0_REG				0xB0
#define SYSTEM_INTERRUPT_CONFIG_GPIO_REG					0x0A
#define SYSTEM_SEQUENCE_CONFIG_REG							0x01 
#define SYSRANGE_START_REG									0x00
#define RESULT_INTERRUPT_STATUS_REG							0x13
#define SYSTEM_INTERRUPT_CLEAR_REG							0x0B
#define GLOBAL_CONFIG_SPAD_ENABLES_REF_0_REG				0xB0
#define GPIO_HV_MUX_ACTIVE_HIGH_REG							0x84
#define SYSTEM_INTERRUPT_CLEAR_REG							0x0B
#define RESULT_RANGE_STATUS_REG								0x14

#define VL53LXX_CONVERSION_INTERVAL 1000 /* 1ms */

#ifndef CONFIG_SCHED_WORKQUEUE
# error This requires CONFIG_SCHED_WORKQUEUE.
#endif

class VL53LXX : public device::I2C
{
public:
	VL53LXX(uint8_t rotation = distance_sensor_s::ROTATION_DOWNWARD_FACING,
		   int bus = VL53LXX_BUS, int address = VL53LXX_BASEADDR);
	virtual ~VL53LXX();

	virtual int 		init();

	//virtual ssize_t		read(device::file_t *filp, char *buffer, size_t buflen);
	virtual int			ioctl(device::file_t *filp, int cmd, unsigned long arg);

	/**
	* Diagnostics - print some basic information about the driver.
	*/
	void				print_info();

private:
	uint8_t _rotation;
	float				_min_distance;
	float				_max_distance;
	work_s				_work;
	ringbuffer::RingBuffer		*_reports;
	bool				_sensor_ok;
	uint8_t				_valid;
	int					_measure_ticks;
	bool				_collect_phase;
	int					_class_instance;
	int					_orb_class_instance;

	orb_advert_t		_distance_sensor_topic;

	perf_counter_t		_sample_perf;
	perf_counter_t		_comms_errors;

	uint8_t 			_multi_read_value[6];

	uint8_t 			stop_variable_;


	/**
	* Initialise the automatic measurement state machine and start it.
	*
	* @note This function is called at open and error time.  It might make sense
	*       to make it more aggressive about resetting the bus in case of errors.
	*/
	void				start();

	/**
	* Stop the automatic measurement state machine.
	*/
	void				stop();

	/**
	* Perform a poll cycle; collect from the previous measurement
	* and start a new one.
	*/
	void				cycle();
	int					measure();
	int					collect();

	int 				readRegister(uint8_t reg_address, uint8_t &value);
	int 				writeRegister(uint8_t reg_address, uint8_t value);
	int 				writeRegisterMulti(uint8_t reg_address, uint8_t* value, uint8_t length);
	//int 				readRegisterMulti(uint8_t reg_address, uint8_t length);
	int 				readRegisterMulti(uint8_t reg_address, uint8_t *value, uint8_t length);

	int 				sensorInit();
	bool 				spadCalculations();
	bool 				sensorTuning();
	bool				singleRefCalibration(uint8_t byte);

	/**
	* Static trampoline from the workq context; because we don't have a
	* generic workq wrapper yet.
	*
	* @param arg		Instance pointer for the driver that is polling.
	*/
	static void		cycle_trampoline(void *arg);


};


/*
 * Driver 'main' command.
 */
extern "C" __EXPORT int vl53lxx_main(int argc, char *argv[]);

VL53LXX::VL53LXX(uint8_t rotation, int bus, int address) :
	I2C("VL53LXX", VL53LXX_DEVICE_PATH, bus, address, 100000),
	_rotation(rotation),
	_min_distance(-1.0f),
	_max_distance(-1.0f),
	_reports(nullptr),
	_sensor_ok(false),
	_valid(0),
	_measure_ticks(0),
	_collect_phase(false),
	_class_instance(-1),
	_orb_class_instance(-1),
	_distance_sensor_topic(nullptr),
	_sample_perf(perf_alloc(PC_ELAPSED, "tr1_read")),
	_comms_errors(perf_alloc(PC_COUNT, "tr1_com_err"))
{
	// up the retries since the device misses the first measure attempts
	I2C::_retries = 3;

	// enable debug() calls
	_debug_enabled = false;

	// work_cancel in the dtor will explode if we don't do this...
	memset(&_work, 0, sizeof(_work));
}

VL53LXX::~VL53LXX()
{
	/* make sure we are truly inactive */
	stop();

	/* free any existing reports */
	if (_reports != nullptr) {
		delete _reports;
	}

	if (_class_instance != -1) {
		unregister_class_devname(RANGE_FINDER_BASE_DEVICE_PATH, _class_instance);
	}

	// free perf counters
	perf_free(_sample_perf);
	perf_free(_comms_errors);
}


int 
VL53LXX::sensorInit()
{
	uint8_t val = 0;
	int ret;
	float rate_limit;
	uint8_t rate_limit_split[2];

	// I2C at 2.8V on sensor side of level shifter
	readRegister(VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HW_REG, val);
	writeRegister(VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HW_REG, val | 0x01);

	// set I2C to standard mode 
	writeRegister(0x88, 0x00);

	writeRegister(0x80, 0x01);
	writeRegister(0xFF, 0x01);
	writeRegister(0x00, 0x00);
	readRegister(0x91, val);
	writeRegister(0x00, 0x01);
	writeRegister(0xFF, 0x00);
	writeRegister(0x80, 0x00);

	stop_variable_ = val;

	// disable SIGNAL_RATE_MSRC (bit 1) and SIGNAL_RATE_PRE_RANGE (bit 4) limit checks
	readRegister(MSRC_CONFIG_CONTROL_REG, val);
	writeRegister(MSRC_CONFIG_CONTROL_REG, val | 0x12);

	// Set signal rate limit to 0.1
	rate_limit = 0.1 * (65536);
	rate_limit_split[0] = (((uint16_t)rate_limit) >> 8);
	rate_limit_split[1] = ((uint16_t)rate_limit);

	writeRegisterMulti(FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT_REG, &rate_limit_split[0], 2);
	writeRegister(SYSTEM_SEQUENCE_CONFIG_REG, 0xFF);

	spadCalculations();

	ret = OK;

	return ret;

}

int
VL53LXX::init()
{
	int ret = PX4_ERROR;

	set_device_address(VL53LXX_BASEADDR);

	/* do I2C init (and probe) first */
	if (I2C::init() != OK) {
		goto out;
	}

	sensorInit();

	/* allocate basic report buffers */
	_reports = new ringbuffer::RingBuffer(2, sizeof(distance_sensor_s));

	if (_reports == nullptr) {
		goto out;
	}

	_class_instance = register_class_devname(RANGE_FINDER_BASE_DEVICE_PATH);

	if (_class_instance == CLASS_DEVICE_PRIMARY) {
		/* get a publish handle on the range finder topic */
		struct distance_sensor_s ds_report;
		//measure();
		_reports->get(&ds_report);

		_distance_sensor_topic = orb_advertise_multi(ORB_ID(distance_sensor), &ds_report,
					 &_orb_class_instance, ORB_PRIO_LOW);

		if (_distance_sensor_topic == nullptr) {
			DEVICE_LOG("failed to create distance_sensor object. Did you start uOrb?");
		}
	}

	ret = OK;
	/* sensor is ok, but we don't really know if it is within range */
	_sensor_ok = true;

out:
	return ret;
}


int
VL53LXX::ioctl(device::file_t *filp, int cmd, unsigned long arg)
{
	printf("ioctl\n");
	switch (cmd) {

	case SENSORIOCSPOLLRATE: {
			switch (arg) {

				case SENSOR_POLLRATE_DEFAULT: {
						/* do we need to start internal polling? */
						//bool want_start = (_measure_ticks == 0);

						/* set interval for next measurement to minimum legal value */
						_measure_ticks = USEC2TICK(VL53LXX_CONVERSION_INTERVAL);

						/* if we need to start the poll state machine, do it */
						// if (want_start) {
						// 	start();
						// }
						start();

						return OK;
				}

				/* adjust to a legal polling interval in Hz */
				default: {
						/* do we need to start internal polling? */
						//bool want_start = (_measure_ticks == 0);

						/* convert hz to tick interval via microseconds */
						unsigned ticks = USEC2TICK(1000000 / arg);

						/* update interval for next measurement */
						_measure_ticks = ticks;

						/* if we need to start the poll state machine, do it */
						//if (want_start) {
							start();
						//}

						return OK;
					}
				}
			}

	default:
		/* give it to the superclass */
		return I2C::ioctl(filp, cmd, arg);
	}
}

// ssize_t
// VL53LXX::read(device::file_t *filp, char *buffer, size_t buflen)
// {
// 	unsigned count = buflen / sizeof(struct distance_sensor_s);
// 	struct distance_sensor_s *rbuf = reinterpret_cast<struct distance_sensor_s *>(buffer);
// 	int ret = 0;

// 	/* buffer must be large enough */
// 	if (count < 1) {
// 		return -ENOSPC;
// 	}

// 	/* if automatic measurement is enabled */
// 	if (_measure_ticks > 0) {

// 		/*
// 		 * While there is space in the caller's buffer, and reports, copy them.
// 		 * Note that we may be pre-empted by the workq thread while we are doing this;
// 		 * we are careful to avoid racing with them.
// 		 */
// 		while (count--) {
// 			if (_reports->get(rbuf)) {
// 				ret += sizeof(*rbuf);
// 				rbuf++;
// 			}
// 		}

// 		/* if there was no data, warn the caller */
// 		return ret ? ret : -EAGAIN;
// 	}

// 	/* manual measurement - run one conversion */
// 	do {
// 		_reports->flush();

// 		/* trigger a measurement */
// 		if (OK != measure()) {
// 			ret = -EIO;
// 			break;
// 		}

// 		/* wait for it to complete */
// 		usleep(VL53LXX_CONVERSION_INTERVAL);

// 		/* run the collection phase */
// 		if (OK != collect()) {
// 			ret = -EIO;
// 			break;
// 		}

// 		/* state machine will have generated a report, copy it out */
// 		if (_reports->get(rbuf)) {
// 			ret = sizeof(*rbuf);
// 		}

// 	} while (0);

// 	return ret;
// }


int 
VL53LXX::readRegister(uint8_t reg_address, uint8_t &value)
{
	int ret;
	uint8_t val = 0;
	ret = transfer(&reg_address, sizeof(reg_address), nullptr, 0);

	if (OK != ret) {
		DEVICE_LOG("i2c::transfer returned %d", ret);
		return ret;
	}

	/* wait for it to complete */
	//usleep(VL53LXX_CONVERSION_INTERVAL);		// maybe not needed ???

	/* read from the sensor */
	ret = transfer(nullptr, 0, &val, 1);

	if (ret < 0) {
		DEVICE_LOG("error reading from sensor: %d", ret);
		perf_count(_comms_errors);
		perf_end(_sample_perf);
		return ret;
	}

	ret = OK;
	value = val;

	return ret;

}


int 
VL53LXX::readRegisterMulti(uint8_t reg_address, uint8_t *value, uint8_t length)
{
	int ret;
	uint8_t val[6] = {0, 0, 0, 0, 0, 0}; 
	ret = transfer(&reg_address, sizeof(reg_address), nullptr, 0);

	if (OK != ret) {
		DEVICE_LOG("i2c::transfer returned %d", ret);
		return ret;
	}

	/* wait for it to complete */
	usleep(VL53LXX_CONVERSION_INTERVAL);		// maybe not needed ???

	/* read from the sensor */
	ret = transfer(nullptr, 0, &val[0], length);

	if (ret < 0) {
		DEVICE_LOG("error reading from sensor: %d", ret);
		perf_count(_comms_errors);
		perf_end(_sample_perf);
		return ret;
	}


	for (int i = 0; i < length; ++i)		// use memcpy
	{
		value[i] = val[i];
		printf("_multi_read_value[%d]: %u\n", i, (uint16_t)_multi_read_value[i] );
	}

	ret = OK;
	
	return ret;

}


int 
VL53LXX::writeRegister(uint8_t reg_address, uint8_t value)
{
	int ret;
	uint8_t cmd[2] = {0, 0};

	cmd[0] = reg_address;
	cmd[1] = value;

	ret = transfer(&cmd[0], 2, nullptr, 0);

	if (OK != ret) {
		DEVICE_LOG("i2c::transfer returned %d", ret);
		return ret;
	}

	ret = OK;

	return ret;

}


int 
VL53LXX::writeRegisterMulti(uint8_t reg_address, uint8_t* value, uint8_t length) //	bytes are send in order as they are in the array
{																				 // be careful for uint16_t to send first higher byte
	int ret;
	uint8_t cmd[6] = {0, 0, 0, 0, 0, 0};

	cmd[0] = reg_address;

	for (int i = 0; i < length; i++) //use memcpy
	{
		cmd[i+1] = value[i];
	}
	
	
	ret = transfer(&cmd[0], length+1, nullptr, 0);

	if (OK != ret) {
		DEVICE_LOG("i2c::transfer returned %d", ret);
		return ret;
	}

	ret = OK;

	return ret;

}


int
VL53LXX::measure()
{
	int ret;
	uint8_t wait_for_measurement;
	uint8_t system_start;

	/*
	 * Send the command to begin a measurement.
	 */
	const uint8_t cmd = RESULT_RANGE_STATUS_REG + 10;

	writeRegister(0x80, 0x01);
	writeRegister(0xFF, 0x01);
	writeRegister(0x00, 0x00);
	writeRegister(0x91, stop_variable_);
	writeRegister(0x00, 0x01);
	writeRegister(0xFF, 0x00);
	writeRegister(0x80, 0x00);

	writeRegister(SYSRANGE_START_REG, 0x01);


	readRegister(SYSRANGE_START_REG, system_start);
	while((system_start & 0x01) == 1){
		readRegister(SYSRANGE_START_REG, system_start);
	}



	readRegister(RESULT_INTERRUPT_STATUS_REG, wait_for_measurement);
	while((wait_for_measurement & 0x07) == 0){
		readRegister(RESULT_INTERRUPT_STATUS_REG, wait_for_measurement);
	}


	ret = transfer(&cmd, sizeof(cmd), nullptr, 0);

	if (OK != ret) {
		perf_count(_comms_errors);
		DEVICE_LOG("i2c::transfer returned %d", ret);
		return ret;
	}

	ret = OK;

	return ret;
}


int
VL53LXX::collect()
{
	int ret = -EIO;

	/* read from the sensor */
	uint8_t val[2] = {0, 0};

	perf_begin(_sample_perf);

	ret = transfer(nullptr, 0, &val[0], 2);

	if (ret < 0) {
		DEVICE_LOG("error reading from sensor: %d", ret);
		perf_count(_comms_errors);
		perf_end(_sample_perf);
		return ret;
	}

	uint16_t distance_mm = (val[0] << 8) | val[1];
	float distance_m = float(distance_mm) *  1e-3f;
	struct distance_sensor_s report;

	report.timestamp = hrt_absolute_time(); 
	report.type = distance_sensor_s::MAV_DISTANCE_SENSOR_LASER;
	report.orientation = _rotation;
	report.current_distance = distance_m;
	report.min_distance = 0.0f;
	report.max_distance = 2.0f;
	report.covariance = 0.0f;
	/* TODO: set proper ID */
	report.id = 0;

	/* publish it, if we are the primary */
	if (_distance_sensor_topic != nullptr) {
		orb_publish(ORB_ID(distance_sensor), _distance_sensor_topic, &report);
	}

	_reports->force(&report);

	/* notify anyone waiting for data */
	poll_notify(POLLIN);

	ret = OK;

	perf_end(_sample_perf);
	return ret;
}

void
VL53LXX::start()
{
	/* reset the report ring and state machine */
	_collect_phase = false;
	_reports->flush();

	/* schedule a cycle to start things */
	work_queue(HPWORK, &_work, (worker_t)&VL53LXX::cycle_trampoline, this, 1000);

	/* notify about state change */
	struct subsystem_info_s info = {};
	info.present = true;
	info.enabled = true;
	info.ok = true;
	info.subsystem_type = subsystem_info_s::SUBSYSTEM_TYPE_RANGEFINDER;

	static orb_advert_t pub = nullptr;

	if (pub != nullptr) {
		orb_publish(ORB_ID(subsystem_info), pub, &info);

	} else {
		pub = orb_advertise(ORB_ID(subsystem_info), &info);
	}
}

void
VL53LXX::stop()
{
	work_cancel(HPWORK, &_work);
}

void
VL53LXX::cycle_trampoline(void *arg)
{
	VL53LXX *dev = (VL53LXX *)arg;

	dev->cycle();
}

void
VL53LXX::cycle()
{
	measure();
	collect();

	/* schedule a fresh cycle call when the measurement is done */
	work_queue(HPWORK,
		   &_work,
		   (worker_t)&VL53LXX::cycle_trampoline,
		   this,
		   USEC2TICK(10000));

}

void
VL53LXX::print_info()
{
	perf_print_counter(_sample_perf);
	perf_print_counter(_comms_errors);
	printf("poll interval:  %u ticks\n", _measure_ticks);
	_reports->print_info("report queue");
}


bool 
VL53LXX::spadCalculations()
{
  uint8_t val;

  uint8_t spad_count;
  bool spad_type_is_aperture;

  uint8_t ref_spad_map[6];

  writeRegister(0x80, 0x01);
  writeRegister(0xFF, 0x01);
  writeRegister(0x00, 0x00);

  writeRegister(0xFF, 0x06);

  readRegister(0x83, val);
  writeRegister(0x83, val | 0x04);
  writeRegister(0xFF, 0x07);
  writeRegister(0x81, 0x01);

  writeRegister(0x80, 0x01);

  writeRegister(0x94, 0x6b);
  writeRegister(0x83, 0x00);

  readRegister(0x83, val);

  while (val == 0x00){
  	readRegister(0x83, val);
  }

  writeRegister(0x83, 0x01);
  readRegister(0x92, val);

  spad_count = val & 0x7f;
  spad_type_is_aperture = (val >> 7) & 0x01;

  writeRegister(0x81, 0x00);
  writeRegister(0xFF, 0x06);
  readRegister(0x83, val);
  writeRegister(0x83, val  & ~0x04);
  writeRegister(0xFF, 0x01);
  writeRegister(0x00, 0x01);

  writeRegister(0xFF, 0x00);
  writeRegister(0x80, 0x00);
  
  readRegisterMulti(GLOBAL_CONFIG_SPAD_ENABLES_REF_0_REG, &ref_spad_map[0], 6);

  writeRegister(0xFF, 0x01);
  writeRegister(DYNAMIC_SPAD_REF_EN_START_OFFSET_REG, 0x00);
  writeRegister(DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD_REG, 0x2C);
  writeRegister(0xFF, 0x00);
  writeRegister(GLOBAL_CONFIG_REF_EN_START_SELECT_REG, 0xB4);

  uint8_t first_spad_to_enable = spad_type_is_aperture ? 12 : 0;
  uint8_t spads_enabled = 0;

  for (uint8_t i = 0; i < 48; i++)
  {
    if (i < first_spad_to_enable || spads_enabled == spad_count)
    {
      ref_spad_map[i / 8] &= ~(1 << (i % 8));
    }
    else if ((ref_spad_map[i / 8] >> (i % 8)) & 0x1)
    {
      spads_enabled++;
    }
  }

  writeRegisterMulti(GLOBAL_CONFIG_SPAD_ENABLES_REF_0_REG, &ref_spad_map[0], 6);

  sensorTuning();

  writeRegister(SYSTEM_INTERRUPT_CONFIG_GPIO_REG, 4); 		// 4: GPIO interrupt on new data 
																
  readRegister(GPIO_HV_MUX_ACTIVE_HIGH_REG, val);
  writeRegister(GPIO_HV_MUX_ACTIVE_HIGH_REG, val & ~0x10);  // active low
  writeRegister(SYSTEM_INTERRUPT_CLEAR_REG, 0x01);
  
  writeRegister(SYSTEM_SEQUENCE_CONFIG_REG, 0xE8);

  writeRegister(SYSTEM_SEQUENCE_CONFIG_REG, 0x01);
  singleRefCalibration(0x40);
  
  writeRegister(SYSTEM_SEQUENCE_CONFIG_REG, 0x02);
  singleRefCalibration(0x00);

  writeRegister(SYSTEM_SEQUENCE_CONFIG_REG, 0xE8); // restore config


  return true;

}


bool 
VL53LXX::sensorTuning()
{

  writeRegister(0xFF, 0x01);
  writeRegister(0x00, 0x00);

  writeRegister(0xFF, 0x00);
  writeRegister(0x09, 0x00);
  writeRegister(0x10, 0x00);
  writeRegister(0x11, 0x00);

  writeRegister(0x24, 0x01);
  writeRegister(0x25, 0xFF);
  writeRegister(0x75, 0x00);

  writeRegister(0xFF, 0x01);
  writeRegister(0x4E, 0x2C);
  writeRegister(0x48, 0x00);
  writeRegister(0x30, 0x20);

  writeRegister(0xFF, 0x00);
  writeRegister(0x30, 0x09);
  writeRegister(0x54, 0x00);
  writeRegister(0x31, 0x04);
  writeRegister(0x32, 0x03);
  writeRegister(0x40, 0x83);
  writeRegister(0x46, 0x25);
  writeRegister(0x60, 0x00);
  writeRegister(0x27, 0x00);
  writeRegister(0x50, 0x06);
  writeRegister(0x51, 0x00);
  writeRegister(0x52, 0x96);
  writeRegister(0x56, 0x08);
  writeRegister(0x57, 0x30);
  writeRegister(0x61, 0x00);
  writeRegister(0x62, 0x00);
  writeRegister(0x64, 0x00);
  writeRegister(0x65, 0x00);
  writeRegister(0x66, 0xA0);

  writeRegister(0xFF, 0x01);
  writeRegister(0x22, 0x32);
  writeRegister(0x47, 0x14);
  writeRegister(0x49, 0xFF);
  writeRegister(0x4A, 0x00);

  writeRegister(0xFF, 0x00);
  writeRegister(0x7A, 0x0A);
  writeRegister(0x7B, 0x00);
  writeRegister(0x78, 0x21);

  writeRegister(0xFF, 0x01);
  writeRegister(0x23, 0x34);
  writeRegister(0x42, 0x00);
  writeRegister(0x44, 0xFF);
  writeRegister(0x45, 0x26);
  writeRegister(0x46, 0x05);
  writeRegister(0x40, 0x40);
  writeRegister(0x0E, 0x06);
  writeRegister(0x20, 0x1A);
  writeRegister(0x43, 0x40);

  writeRegister(0xFF, 0x00);
  writeRegister(0x34, 0x03);
  writeRegister(0x35, 0x44);

  writeRegister(0xFF, 0x01);
  writeRegister(0x31, 0x04);
  writeRegister(0x4B, 0x09);
  writeRegister(0x4C, 0x05);
  writeRegister(0x4D, 0x04);

  writeRegister(0xFF, 0x00);
  writeRegister(0x44, 0x00);
  writeRegister(0x45, 0x20);
  writeRegister(0x47, 0x08);
  writeRegister(0x48, 0x28);
  writeRegister(0x67, 0x00);
  writeRegister(0x70, 0x04);
  writeRegister(0x71, 0x01);
  writeRegister(0x72, 0xFE);
  writeRegister(0x76, 0x00);
  writeRegister(0x77, 0x00);

  writeRegister(0xFF, 0x01);
  writeRegister(0x0D, 0x01);

  writeRegister(0xFF, 0x00);
  writeRegister(0x80, 0x01);
  writeRegister(0x01, 0xF8);

  writeRegister(0xFF, 0x01);
  writeRegister(0x8E, 0x01);
  writeRegister(0x00, 0x01);
  writeRegister(0xFF, 0x00);
  writeRegister(0x80, 0x00);

  return true;
}


bool
VL53LXX::singleRefCalibration(uint8_t byte)
{
  uint8_t val;

  writeRegister(SYSRANGE_START_REG, byte | 0x01); 		// VL53L0X_REG_SYSRANGE_MODE_START_STOP	

  do 
  {
  	readRegister(RESULT_INTERRUPT_STATUS_REG, val);
  } 
  while ((val & 0x07) == 0);

  writeRegister(SYSTEM_INTERRUPT_CLEAR_REG, 0x01);

  writeRegister(SYSRANGE_START_REG, 0x00);

  return true;
}



/**
 * Local functions in support of the shell command.
 */
namespace vl53lxx
{

VL53LXX	*g_dev;

void	start(uint8_t rotation);
void	stop();
void	test();
void	reset();
void	info();

/**
 * Start the driver.
 */
void
start(uint8_t rotation)
{
	int fd;

	if (g_dev != nullptr) {
		errx(1, "already started");
	}

	/* create the driver */
	g_dev = new VL53LXX(rotation, VL53LXX_BUS);


	if (g_dev == nullptr) {
		goto fail;
	}

	if (OK != g_dev->init()) {
		goto fail;
	}

	/* set the poll rate to default, starts automatic data collection */
	fd = open(VL53LXX_DEVICE_PATH, O_RDONLY);

	if (fd < 0) {
		goto fail;
	}

	if (ioctl(fd, SENSORIOCSPOLLRATE, SENSOR_POLLRATE_DEFAULT) < 0) {
		goto fail;
	}

	exit(0);

fail:

	if (g_dev != nullptr) {
		delete g_dev;
		g_dev = nullptr;
	}

	errx(1, "driver start failed");
}

/**
 * Stop the driver
 */
void stop()
{
	if (g_dev != nullptr) {
		delete g_dev;
		g_dev = nullptr;

	} else {
		errx(1, "driver not running");
	}

	exit(0);
}

/**
 * Perform some basic functional tests on the driver;
 * make sure we can collect data from the sensor in polled
 * and automatic modes.
 */
void
test()
{
	struct distance_sensor_s report;

	if (g_dev != nullptr) {
		errx(1, "already started");
	}

	/* create the driver */
	g_dev = new VL53LXX(0, VL53LXX_BUS);

	if (g_dev == nullptr) {
		delete g_dev;
		g_dev = nullptr;
	}

	if (OK != g_dev->init()) {
		delete g_dev;
		g_dev = nullptr;
	
	}

	int fd = open(VL53LXX_DEVICE_PATH, O_RDONLY);

	if (fd < 0) {
		err(1, "%s open failed (try 'vl53lxx start' if the driver is not running", VL53LXX_DEVICE_PATH);
	}

	warnx("single read");
	warnx("measurement: %0.2f m", (double)report.current_distance);
	warnx("time:        %llu", report.timestamp);

	/* start the sensor polling at 2Hz */
	if (OK != ioctl(fd, SENSORIOCSPOLLRATE, SENSOR_POLLRATE_DEFAULT)) {
		errx(1, "failed to set 2Hz poll rate");
	}

	errx(0, "PASS");
}

/**
 * Reset the driver.
 */
void
reset()
{
	int fd = open(VL53LXX_DEVICE_PATH, O_RDONLY);

	if (fd < 0) {
		err(1, "failed ");
	}

	if (ioctl(fd, SENSORIOCRESET, 0) < 0) {
		err(1, "driver reset failed");
	}

	if (ioctl(fd, SENSORIOCSPOLLRATE, SENSOR_POLLRATE_DEFAULT) < 0) {
		err(1, "driver poll restart failed");
	}

	exit(0);
}

/**
 * Print a little info about the driver.
 */
void
info()
{
	if (g_dev == nullptr) {
		errx(1, "driver not running");
	}

	printf("state @ %p\n", g_dev);
	g_dev->print_info();

	exit(0);
}

} // namespace

int
vl53lxx_main(int argc, char *argv[])
{
	int ch;
	int myoptind = 1;
	const char *myoptarg = nullptr;
	uint8_t rotation = distance_sensor_s::ROTATION_DOWNWARD_FACING;

	while ((ch = px4_getopt(argc, argv, "R:", &myoptind, &myoptarg)) != EOF) {
		switch (ch) {
		case 'R':
			rotation = (uint8_t)atoi(myoptarg);
			PX4_INFO("Setting sensor orientation to %d", (int)rotation);
			break;

		default:
			PX4_WARN("Unknown option!");
		}
	}

	/*
	 * Start/load the driver.
	 */
	if (!strcmp(argv[myoptind], "start")) {
		vl53lxx::start(rotation);
	}

	/*
	 * Stop the driver
	 */
	if (!strcmp(argv[myoptind], "stop")) {
		vl53lxx::stop();
	}

	/*
	 * Test the driver/device.
	 */
	if (!strcmp(argv[myoptind], "test")) {
		vl53lxx::test();
	}

	/*
	 * Reset the driver.
	 */
	if (!strcmp(argv[myoptind], "reset")) {
		vl53lxx::reset();
	}

	/*
	 * Print driver information.
	 */
	if (!strcmp(argv[myoptind], "info") || !strcmp(argv[1], "status")) {
		vl53lxx::info();
	}

	PX4_ERR("unrecognized command, try 'start', 'test', 'reset' or 'info'");
	return PX4_ERROR;
}
