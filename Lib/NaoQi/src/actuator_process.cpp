#include "actuator_process.h"
#include "shm_util.h"

#include "alcommon/albroker.h"
#include "alproxies/dcmproxy.h"
#include "almemoryfastaccess/almemoryfastaccess.h"

#include <vector>
#include <string>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>

using namespace AL;

static DCMProxy *dcm;

typedef struct {
  unsigned int size;
  std::vector<double *> shmPtrs;
  ALValue dcmCommand;
  int priority;
} structCommand;

static std::vector<structCommand> commands;

static double *actuatorPtrDisable;
static double *actuatorPtrCount;
static double *actuatorPtrPosition;
static double *actuatorPtrCommand;
static double *actuatorPtrVelocity;
static double *actuatorPtrHardness;

int actuator_process_init(ALPtr<ALBroker> pBroker) {
  ALMemoryFastAccess memoryFastAccess;
  try {
    dcm = new DCMProxy(pBroker);
  } catch(ALError &e) {
    std::cerr << "Could not open dcm: " << e.toString() << std::endl;
    return -1;
  }

  int ret = actuator_shm_open();
  if (ret) {
    std::cerr << "Could not open actuator shm" << std::endl;
    return ret;
  }

  commands.clear();
  // Setup corresponding DCM and SHM pointers
  struct structActuatorKeys *actuatorKey = actuatorKeys;
  while (actuatorKey->key != NULL) {
    printf("Setting up actuator key: %s[%d]\n", actuatorKey->key, actuatorKey->size);

    int nval = actuatorKey->size;
    // Pointer in shared memory
    double *pr = actuator_shm_set_ptr(actuatorKey->key, nval);
    std::string key(actuatorKey->key);

    // If there are corresponding DCM names, setup DCM access arrays:
    if (actuatorKey->names != NULL) {
      structCommand cmd;
      cmd.size = nval;
      cmd.shmPtrs.resize(nval);
      for (int i = 0; i < nval; i++) {
        cmd.shmPtrs[i] = pr + i;
      }

      // led and us are low priority commands
      if (key.find("led") != std::string::npos) {
        cmd.priority = 10;
      }
      else if (key.find("us") != std::string::npos) {
			//cmd.priority = 20;
			cmd.priority = 100;
      }
      else {
        cmd.priority = 1;
      }

      // Setup DCM alias:
      ALValue alias;
      alias.arraySetSize(2);
      alias[0] = key;
      alias[1].arraySetSize(nval);
      for (int i = 0; i < nval; i++)
        alias[1][i] = std::string(actuatorKey->names[i]);
      try {
        dcm->createAlias(alias);
      } catch (ALError &e) {
        std::cerr << "Could not create DCM alias: " << e.toString() << std::endl;
      }

      // Setup DCM command values:
      cmd.dcmCommand.arraySetSize(6);
      cmd.dcmCommand[0] = key;
      // "ClearAll" is much slower than "Merge"
      // when commands are added in the future
      cmd.dcmCommand[1] = "Merge";
      cmd.dcmCommand[2] = "time-separate";
      cmd.dcmCommand[3] = 0;
      cmd.dcmCommand[4].arraySetSize(1);
      cmd.dcmCommand[5].arraySetSize(nval);
      for (int i = 0; i < nval; i++)
				cmd.dcmCommand[5][i].arraySetSize(1);
      commands.push_back(cmd);
    }

    actuatorKey++;
  }

  actuatorPtrDisable = actuator_shm_get_ptr("disable");
  actuatorPtrCommand = actuator_shm_get_ptr("command");
  actuatorPtrVelocity = actuator_shm_get_ptr("velocity");
  actuatorPtrPosition = actuator_shm_get_ptr("position");
  actuatorPtrHardness = actuator_shm_get_ptr("hardness");
  actuatorPtrCount = actuator_shm_get_ptr("count");

  printf("Number of commands arrays: %d\n", commands.size());

  return 0;
}

int actuator_process() {
  static unsigned int count = 0;
  static int updateCount = 0;
  count++;

  struct timeval tv;
  gettimeofday(&tv, NULL);
  //  double t = tv.tv_sec + 1E-6*tv.tv_usec;
  int tDcm = (unsigned int)(tv.tv_usec/1000) + (unsigned int)(tv.tv_sec*1000);

  if (*actuatorPtrDisable > 0) {
    return 1;
  }

  for (int i = 0; i < nJoint; i++) {
    double hardness = actuatorPtrHardness[i];
    if (hardness <= 0) continue;

    double vel = actuatorPtrVelocity[i];
    if (vel > 0) {
      double delta = actuatorPtrCommand[i] - actuatorPtrPosition[i];
      double deltaMax = 0.010*vel;
      if (delta > deltaMax) {
        delta = deltaMax;
      } else if (delta < -deltaMax) {
        delta = -deltaMax;
      }
      actuatorPtrPosition[i] += delta;

    } else {
      actuatorPtrPosition[i] = actuatorPtrCommand[i];
    }
  }

  //if (updateCount != updateCount) {
  for (unsigned int k = 0; k < commands.size(); k++) {
    // Copy shared memory values to DCM commands:
    structCommand &cmd = commands[k];

    // Periodically skip low priority commands
    if ((count % cmd.priority) != 0) continue;

    cmd.dcmCommand[4][0] = tDcm + 10;
    for (unsigned int i = 0; i < cmd.size; i++) {
      cmd.dcmCommand[5][i][0] = *cmd.shmPtrs[i];
    }

    // Send DCM command
    try {
      dcm->setAlias(cmd.dcmCommand);
    } catch (ALError& e) {
      std::cerr << "Could not send DCM command: " << e.toString() << std::endl;
    }

    updateCount = *actuatorPtrCount;
  }
  //} else {
    
    
  //}
  
  return 0;
}

int actuator_process_exit() {
  actuator_shm_close();
  if (dcm) 
    delete dcm;

  return 0;
}
