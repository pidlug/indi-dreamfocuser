/*
  INDI Driver for DreamFocuser

  Copyright (C) 2016 Piotr Dlugosz

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <memory>
#include <indicom.h>

#include "dreamfocuser.h"
#include "connectionplugins/connectionserial.h"

#define FOCUS_SETTINGS_TAB  "Settings"
#define FOCUS_STATUS_TAB    "Status"


/*
COMMANDS:

MMabcd0z - set position x
response - MMabcd0z

MH00000z - stop
response - MH00000z

MP00000z - read position
response - MPabcd0z

MI00000z - is moving
response - MI000d0z - d = 1: yes, 0: no

MT00000z - read temperature
response - MT00cd0z - temperature = ((c<<8)|d)/16.0

----

MR000d0z - move with speed d & 0b1111111 (0 - 127), direction d >> 7 (1 up, 0 down)
response - MR000d0z

MW00000z - is calibrated
response - MW000d0z - d = 1: yes (absolute mode), 0: no (relative mode)

MZabcd0z - calibrate toposition x
response - MZabcd0z

MV00000z - firmware version
response - MV00cd0z - version: c.d

MG00000z - park
response - MG00000z
*/

const int POLLMS = 500;

// We declare an auto pointer to DreamFocuser.
std::unique_ptr<DreamFocuser> dreamFocuser(new DreamFocuser());

void ISPoll(void *p);

void ISGetProperties(const char *dev)
{
    dreamFocuser->ISGetProperties(dev);
}

void ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int num)
{
    dreamFocuser->ISNewSwitch(dev, name, states, names, num);
}

void ISNewText(	const char *dev, const char *name, char *texts[], char *names[], int num)
{
    dreamFocuser->ISNewText(dev, name, texts, names, num);
}

void ISNewNumber(const char *dev, const char *name, double values[], char *names[], int num)
{
    dreamFocuser->ISNewNumber(dev, name, values, names, num);
}

void ISNewBLOB (const char *dev, const char *name, int sizes[], int blobsizes[], char *blobs[], char *formats[], char *names[], int n)
{
  INDI_UNUSED(dev);
  INDI_UNUSED(name);
  INDI_UNUSED(sizes);
  INDI_UNUSED(blobsizes);
  INDI_UNUSED(blobs);
  INDI_UNUSED(formats);
  INDI_UNUSED(names);
  INDI_UNUSED(n);
}

void ISSnoopDevice (XMLEle *root)
{
    dreamFocuser->ISSnoopDevice(root);
}


/****************************************************************
**
**
*****************************************************************/

DreamFocuser::DreamFocuser()
{
  FI::SetCapability(FOCUSER_CAN_ABS_MOVE | FOCUSER_CAN_REL_MOVE | FOCUSER_CAN_ABORT);

  isAbsolute = false;
  isMoving = false;

}

DreamFocuser::~DreamFocuser()
{
}

bool DreamFocuser::initProperties()
{
  INDI::Focuser::initProperties();

  // Default speed
  //FocusSpeedN[0].min = 0;
  //FocusSpeedN[0].max = 127;
  //FocusSpeedN[0].value = 50;
  //IUUpdateMinMax(&FocusSpeedNP);

  // Max Position
  IUFillNumber(&MaxPositionN[0], "MAXPOSITION", "Ticks", "%.f", 1., 500000., 1000., 300000);
  IUFillNumberVector(&MaxPositionNP, MaxPositionN, 1, getDeviceName(), "MAXPOSITION", "Max Absolute Position", FOCUS_SETTINGS_TAB, IP_RW, 0, IPS_IDLE);

  IUFillNumber(&MaxTravelN[0], "MAXTRAVEL", "Ticks", "%.f", 1., 500000., 1000., 300000.);
  IUFillNumberVector(&MaxTravelNP, MaxTravelN, 1, getDeviceName(), "MAXTRAVEL", "Max Relative Travel", FOCUS_SETTINGS_TAB, IP_RW, 0, IPS_IDLE );


  // Focus Sync
  IUFillSwitch(&SyncS[0], "SYNC", "Synchronize", ISS_OFF);
  IUFillSwitchVector(&SyncSP, SyncS, 1, getDeviceName(), "SYNC", "Synchronize", MAIN_CONTROL_TAB, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

  // Focus Park
  IUFillSwitch(&ParkS[0], "PARK", "Park", ISS_OFF);
  IUFillSwitchVector(&ParkSP, ParkS, 1, getDeviceName(), "PARK", "Park", MAIN_CONTROL_TAB, IP_RW, ISR_ATMOST1, 0, IPS_IDLE);

  // Focuser temperature and humidity
  IUFillNumber(&EnvironmentN[0], "TEMPERATURE", "Temperature [K]", "%6.1f", 0, 999., 0., 0.);
  IUFillNumber(&EnvironmentN[1], "HUMIDITY", "Humidity [%]", "%6.1f", 0, 999., 0., 0.);
  IUFillNumberVector(&EnvironmentNP, EnvironmentN, 2, getDeviceName(), "ATMOSPHERE", "Atmosphere", MAIN_CONTROL_TAB, IP_RO, 0, IPS_IDLE);

  // Focuser humidity
  //IUFillNumberVector(&HumidityNP, HumidityN, 1, getDeviceName(), "HUMIDITY", "Humidity", MAIN_CONTROL_TAB, IP_RO, 0, IPS_IDLE);

  // We init here the property we wish to "snoop" from the target device
  IUFillSwitch(&StatusS[0], "SYNCHRONIZED", "Synchronized", ISS_OFF);
  IUFillSwitch(&StatusS[1], "MOVING", "Moving", ISS_OFF);
  IUFillSwitchVector(&StatusSP, StatusS, 2, getDeviceName(), "STATUS", "Status", MAIN_CONTROL_TAB, IP_RO, ISR_NOFMANY, 0, IPS_IDLE);

  PresetN[0].min = PresetN[1].min = PresetN[2].min = FocusAbsPosN[0].min = -MaxPositionN[0].value;
  PresetN[0].max = PresetN[1].max = PresetN[2].max = FocusAbsPosN[0].max = MaxPositionN[0].value;
  strcpy(PresetN[0].format, "%6.0f");
  strcpy(PresetN[1].format, "%6.0f");
  strcpy(PresetN[2].format, "%6.0f");
  PresetN[0].step = PresetN[1].step = PresetN[2].step = FocusAbsPosN[0].step = DREAMFOCUSER_STEP_SIZE;
  FocusAbsPosN[0].value = 0;

  FocusRelPosN[0].max = MaxTravelN[0].value;
  FocusRelPosN[0].step = DREAMFOCUSER_STEP_SIZE;
  FocusRelPosN[0].value = 5 * DREAMFOCUSER_STEP_SIZE;

  simulatedTemperature = 20.0;
  simulatedHumidity = 1.0;
  simulatedPosition = 2000;

  serialConnection->setDefaultPort("/dev/ttyACM0");
  //strcpy(PortT[0].text, "/dev/ttyACM0");
  //addAuxControls();

  return true;
}

bool DreamFocuser::updateProperties()
{
  INDI::Focuser::updateProperties();

  if (isConnected())
  {
    defineSwitch(&SyncSP);
    defineSwitch(&ParkSP);
    defineNumber(&EnvironmentNP);
    defineSwitch(&StatusSP);
    defineNumber(&MaxPositionNP);
    defineNumber(&MaxTravelNP);
  }
  else
  {
    deleteProperty(SyncSP.name);
    deleteProperty(ParkSP.name);
    deleteProperty(EnvironmentNP.name);
    deleteProperty(StatusSP.name);
    deleteProperty(MaxPositionNP.name);
    deleteProperty(MaxTravelNP.name);
  }
  return true;
}


/*
void DreamFocuser::ISGetProperties(const char *dev)
{
  if(dev && strcmp(dev,getDeviceName()))
  {
    defineNumber(&MaxPositionNP);
    loadConfig(true, "MAXPOSITION");
  };
  return INDI::Focuser::ISGetProperties(dev);
}
*/


bool DreamFocuser::saveConfigItems(FILE *fp)
{
    INDI::Focuser::saveConfigItems(fp);

    IUSaveConfigNumber(fp, &MaxPositionNP);
    IUSaveConfigNumber(fp, &MaxTravelNP);

    return true;
}


bool DreamFocuser::ISNewNumber (const char *dev, const char *name, double values[], char *names[], int n)
{

    if(strcmp(dev,getDeviceName())==0)
    {
        // Max Position
        if (!strcmp(MaxPositionNP.name, name))
        {
            IUUpdateNumber(&MaxPositionNP, values, names, n);

            if (MaxPositionN[0].value > 0)
            {
                PresetN[0].min = PresetN[1].min = PresetN[2].min = FocusAbsPosN[0].min = -MaxPositionN[0].value;;
                PresetN[0].max = PresetN[1].max = PresetN[2].max = FocusAbsPosN[0].max = MaxPositionN[0].value;
                IUUpdateMinMax(&FocusAbsPosNP);
                IUUpdateMinMax(&PresetNP);
                IDSetNumber(&FocusAbsPosNP, NULL);

                DEBUGF(INDI::Logger::DBG_SESSION, "Focuser absolute limits: min (%g) max (%g)", FocusAbsPosN[0].min, FocusAbsPosN[0].max);
            }

            MaxPositionNP.s = IPS_OK;
            IDSetNumber(&MaxPositionNP, NULL);
            return true;
        }


        // Max Travel
        if (!strcmp(MaxTravelNP.name, name))
        {
            IUUpdateNumber(&MaxTravelNP, values, names, n);

            if (MaxTravelN[0].value > 0)
            {
                FocusRelPosN[0].min = 0;
                FocusRelPosN[0].max = MaxTravelN[0].value;
                IUUpdateMinMax(&FocusRelPosNP);
                IDSetNumber(&FocusRelPosNP, NULL);

                DEBUGF(INDI::Logger::DBG_SESSION, "Focuser relative limits: min (%g) max (%g)", FocusRelPosN[0].min, FocusRelPosN[0].max);
            }

            MaxTravelNP.s = IPS_OK;
            IDSetNumber(&MaxTravelNP, NULL);
            return true;
        }

   }

    return INDI::Focuser::ISNewNumber(dev, name, values, names, n);

}


bool DreamFocuser::ISNewSwitch (const char *dev, const char *name, ISState *states, char *names[], int n)
{

    if(strcmp(dev,getDeviceName())==0)
    {
        // Sync
        if (!strcmp(SyncSP.name, name))
        {
            IUResetSwitch(&SyncSP);

            if ( setSync() )
            {
                SyncSP.s = IPS_OK;
                FocusAbsPosNP.s = IPS_OK;
                IDSetNumber(&FocusAbsPosNP, NULL);
            }
            else
                SyncSP.s = IPS_ALERT;
            IDSetSwitch(&SyncSP, NULL);
            return true;
        }
    }

    if(strcmp(dev,getDeviceName())==0)
    {
        // Park
        if (!strcmp(ParkSP.name, name))
        {
            IUResetSwitch(&ParkSP);

            if ( setPark() )
            {
                ParkSP.s = IPS_OK;
                FocusAbsPosNP.s = IPS_OK;
                IDSetNumber(&FocusAbsPosNP, NULL);
            }
            else
                ParkSP.s = IPS_ALERT;
            IDSetSwitch(&ParkSP, NULL);
            return true;
        }
    }

    return INDI::Focuser::ISNewSwitch(dev, name, states, names, n);
}



/****************************************************************
**
**
*****************************************************************/   
bool DreamFocuser::Connect()
{
  if (isConnected())
    return true;

  if (isSimulation())
  {
    DEBUGF(INDI::Logger::DBG_SESSION, "DreamFocuser: Simulating connection to port %s.", serialConnection->port());
    currentPosition = simulatedPosition;
    fd=-1;
    SetTimer(POLLMS);
    return true;
  }

  DEBUG(INDI::Logger::DBG_DEBUG, "Attempting to connect to DreamFocuser focuser....");

  if (tty_connect(serialConnection->port(), 9600, 8, 0, 1, &fd) != TTY_OK)
  {
    DEBUGF(INDI::Logger::DBG_ERROR, "Error connecting to port %s. Make sure you have BOTH read and write permission to the port.", serialConnection->port());
    return false;
  }

  if ( !getStatus() )
    return false;

  DEBUG(INDI::Logger::DBG_SESSION, "Successfully connected to DreamFocuser.");
  SetTimer(POLLMS);

  return true;
}

bool DreamFocuser::Disconnect()
{

  AbortFocuser();
  tty_disconnect(fd);

  return true;
}



/****************************************************************
**
**
*****************************************************************/

bool DreamFocuser::getTemperature()
{
  if ( isSimulation() )
  {
    currentTemperature = simulatedTemperature;
    currentHumidity = simulatedHumidity;
  }
  else
    if ( dispatch_command('T') )
    {
      currentTemperature = ((short int)( (currentResponse.c<<8) | currentResponse.d )) / 10. + 273.15;
      currentHumidity = ((short int)( (currentResponse.a<<8) | currentResponse.b )) / 10.;
    }
    else
      return false;
  return true;
}


bool DreamFocuser::getStatus()
{
  if ( ! isSimulation() ) {
    DEBUG(INDI::Logger::DBG_DEBUG, "getStatus - real, not simualtion.");
    if ( dispatch_command('I') ) // Is moving?
      isMoving = currentResponse.d == 1 ? true : false;
    else
      return false;
    if ( dispatch_command('W') ) // Is absolute?
      isAbsolute = currentResponse.d == 1 ? true : false;
    else
      return false;
  }
  return true;
}


bool DreamFocuser::getPosition()
{
  //int32_t pos;

  if ( isSimulation() )
    currentPosition = simulatedPosition;
  else
    if ( dispatch_command('P') )
      currentPosition = (currentResponse.a<<24) | (currentResponse.b<<16) | (currentResponse.c<<8) | currentResponse.d;
    else
      return false;

  return true;
}


bool DreamFocuser::setPosition( int32_t position)
{
  if ( dispatch_command('M', position) )
    if ( ((currentResponse.a<<24) | (currentResponse.b<<16) | (currentResponse.c<<8) | currentResponse.d) == position )
    {
      DEBUGF(INDI::Logger::DBG_SESSION, "Moving to position %d", position);
      return true;
    };
  return false;
}


bool DreamFocuser::setSync( uint32_t position)
{
  if ( dispatch_command('Z', position) )
    if ( ((currentResponse.a<<24) | (currentResponse.b<<16) | (currentResponse.c<<8) | currentResponse.d) == position )
    {
      DEBUGF(INDI::Logger::DBG_SESSION, "Syncing to position %d", position);
      return true;
    };
  DEBUG(INDI::Logger::DBG_ERROR, "Sync failed.");
  return false;
}

bool DreamFocuser::setPark()
{
  if ( dispatch_command('G') )
  {
    DEBUG(INDI::Logger::DBG_SESSION, "Focuser parked.");
    return true;
  };
  DEBUG(INDI::Logger::DBG_ERROR, "Park failed.");
  return false;
}

bool DreamFocuser::AbortFocuser()
{
  if ( dispatch_command('H') )
  {
    DEBUG(INDI::Logger::DBG_SESSION, "Focusing aborted.");
    return true;
  };
  DEBUG(INDI::Logger::DBG_ERROR, "Abort failed.");
  return false;
}


/*
IPState DreamFocuser::MoveFocuser(FocusDirection dir, int speed, uint16_t duration)
{
  DreamFocuserCommand c;
  unsigned char d = (unsigned char) (speed & 0b01111111) | (dir == FOCUS_INWARD) ? 0b10000000 : 0;

  if ( dispatch_command('R', d) )
  {
    gettimeofday(&focusMoveStart,NULL);
    focusMoveRequest = duration/1000.0;
    if ( read_response() )
      if ( ( currentResponse.k == 'R' ) && (currentResponse.d == d) )
        if (duration <= POLLMS)
        {
          usleep(POLLMS * 1000);
          AbortFocuser();
          return IPS_OK;
        }
        else
          return IPS_BUSY;
  }
  return IPS_ALERT;
}
*/

IPState DreamFocuser::MoveAbsFocuser(uint32_t ticks)
{
//  if ( ! isSynced )
//  {
//    DEBUGF(INDI::Logger::DBG_DEBUG, "Not synced, absolute mode disabled");
//    return IPS_ALERT;
//  }

  DEBUGF(INDI::Logger::DBG_DEBUG, "MoveAbsPosition: %d", ticks);

  if ( setPosition(ticks) ) {
    FocusAbsPosNP.s = IPS_OK;
    IDSetNumber(&FocusAbsPosNP, NULL);
    return IPS_OK;
  }
  return IPS_ALERT;
}

IPState DreamFocuser::MoveRelFocuser(FocusDirection dir, uint32_t ticks)
{
  int32_t finalTicks = currentPosition + ((int32_t)ticks * (dir == FOCUS_INWARD ? -1 : 1));

  DEBUGF(INDI::Logger::DBG_DEBUG, "MoveRelPosition: %d", finalTicks);

  if ( setPosition(finalTicks) ) {
    FocusRelPosNP.s = IPS_OK;
    IDSetNumber(&FocusRelPosNP, NULL);
    return IPS_OK;
  }
  return IPS_ALERT;
}


void DreamFocuser::TimerHit()
{

   if ( ! isConnected() )
     return;
     
   int oldAbsStatus = FocusAbsPosNP.s;
   uint32_t oldPosition = currentPosition;

   if ( getStatus() )
   {

     StatusSP.s = IPS_OK;
     if ( isMoving )
     {
       //DEBUG(INDI::Logger::DBG_SESSION, "Moving" );
       FocusAbsPosNP.s = IPS_BUSY;
       StatusS[1].s = ISS_ON;
     }
     else
     {
       if ( FocusAbsPosNP.s != IPS_IDLE )
         FocusAbsPosNP.s = IPS_OK;
       StatusS[1].s = ISS_OFF;
     };

     if ( isAbsolute )
       StatusS[0].s = ISS_ON;
     else
       StatusS[0].s = ISS_OFF;

     if ( getTemperature() )
     {
       EnvironmentNP.s = ( (EnvironmentN[0].value != currentTemperature) || (EnvironmentN[1].value != currentHumidity)) ? IPS_BUSY : IPS_OK;
       EnvironmentN[0].value = currentTemperature;
       EnvironmentN[1].value = currentHumidity;
     }
     else {
       EnvironmentNP.s = IPS_ALERT;
     }

     if ( FocusAbsPosNP.s != IPS_IDLE )
     {
       if ( getPosition() )
       {
         if ( oldPosition != currentPosition )
         {
           FocusAbsPosNP.s = IPS_BUSY;
           StatusS[1].s = ISS_ON;
           FocusAbsPosN[0].value = currentPosition;
         }
         else
         {
           StatusS[1].s = ISS_OFF;
           FocusAbsPosNP.s = IPS_OK;
         }
         //if ( currentPosition < 0 )
          // FocusAbsPosNP.s = IPS_ALERT;
       }
       else
         FocusAbsPosNP.s = IPS_ALERT;
     }
   }
   else
     StatusSP.s = IPS_ALERT;

   if ((oldAbsStatus != FocusAbsPosNP.s) || (oldPosition != currentPosition))
     IDSetNumber(&FocusAbsPosNP, NULL);

   IDSetNumber(&EnvironmentNP, NULL);
   IDSetSwitch(&SyncSP, NULL);
   IDSetSwitch(&StatusSP, NULL);

   SetTimer(POLLMS);

}


/****************************************************************
**
**
*****************************************************************/

unsigned char DreamFocuser::calculate_checksum(DreamFocuserCommand c)
{
  unsigned char z;

  // calculate checksum
  z = (c.M + c.k + c.a + c.b + c.c + c.d + c.n) & 0xff;
  return z;
}

bool DreamFocuser::send_command(char k, uint32_t l)
{
  DreamFocuserCommand c;
  int err_code = 0, nbytes_written=0;
  char dreamFocuser_error[DREAMFOCUSER_ERROR_BUFFER];
  unsigned char *x = (unsigned char *)&l;

  switch(k)
  {
    case 'M':
    case 'Z':
      c.a = x[3];
      c.b = x[2];
      c.c = x[1];
      c.d = x[0];
      break;
    case 'H':
    case 'P':
    case 'I':
    case 'T':
    case 'W':
    case 'G':
    case 'V':
      c.a = 0;
      c.b = 0;
      c.c = 0;
      c.d = 0;
      break;
    case 'R':
      c.a = 0;
      c.b = 0;
      c.c = 0;
      c.d = x[0];
      break;
    default:
      DEBUGF(INDI::Logger::DBG_ERROR, "Unknown command: '%c'", k);
      return false;
  }
  c.k = k;
  c.n = '\0';
  c.z = calculate_checksum(c);

  DEBUGF(INDI::Logger::DBG_DEBUG, "Sending command: c=%c, a=%hhu, b=%hhu, c=%hhu, d=%hhu ($%hhx), n=%hhu, z=%hhu", c.k, c.a, c.b, c.c, c.d, c.d, c.n, c.z);

  if (isSimulation()) {
    currentResponse.k = k;
    currentResponse.n = '\0';
    currentResponse.z = c.z;
    return true;
  }

  tcflush(fd, TCIOFLUSH);

  if ( (err_code = tty_write(fd, (char *)&c, sizeof(c), &nbytes_written) != TTY_OK))
  {
    tty_error_msg(err_code, dreamFocuser_error, DREAMFOCUSER_ERROR_BUFFER);
    DEBUGF(INDI::Logger::DBG_ERROR, "TTY error detected: %s", dreamFocuser_error);
    return false;
  }

  DEBUGF(INDI::Logger::DBG_DEBUG, "Sending complete. Number of bytes written: %d", nbytes_written);

  return true;
}

bool DreamFocuser::read_response()
{
  int err_code = 0, nbytes_read = 0, z;
  char err_msg[DREAMFOCUSER_ERROR_BUFFER];

  //DEBUG(INDI::Logger::DBG_DEBUG, "Read response");

  if (isSimulation())
  {
     DEBUG(INDI::Logger::DBG_DEBUG, "Simulation!");
     return true;
  }

  // Read a single response
  if ( (err_code = tty_read(fd, (char *)&currentResponse, sizeof(currentResponse), 5, &nbytes_read)) != TTY_OK)
  {
    tty_error_msg(err_code, err_msg, 32);
    DEBUGF(INDI::Logger::DBG_ERROR, "TTY error detected: %s", err_msg);
    return false;
  }
  DEBUGF(INDI::Logger::DBG_DEBUG, "Response: %c, a=%hhu, b=%hhu, c=%hhu, d=%hhu ($%hhx), n=%hhu, z=%hhu", currentResponse.k, currentResponse.a, currentResponse.b, currentResponse.c, currentResponse.d, currentResponse.d, currentResponse.n, currentResponse.z);

  if ( nbytes_read != sizeof(currentResponse) )
  {
    DEBUGF(INDI::Logger::DBG_ERROR, "Number of bytes read: %d, expected: %d", nbytes_read, sizeof(currentResponse));
    return false;
  }

  z = calculate_checksum(currentResponse);
  if ( z != currentResponse.z )
  {
    DEBUGF(INDI::Logger::DBG_ERROR, "Response checksum in not correct %hhu, expected: %hhu", currentResponse.z, z );
    return false;
  }

  if ( currentResponse.k == '!' )
  {
    DEBUG(INDI::Logger::DBG_ERROR, "Focuser reported unrecognized command.");
    return false;
  }

  if ( currentResponse.k == '?' )
  {
    DEBUG(INDI::Logger::DBG_ERROR, "Focuser reported bad checksum.");
    return false;
  }

  return true;
}

bool DreamFocuser::dispatch_command(char k, uint32_t l)
{
  DEBUG(INDI::Logger::DBG_DEBUG, "send_command");
  if ( send_command(k, l) )
  {
    if ( read_response() )
    {
      DEBUG(INDI::Logger::DBG_DEBUG, "check currentResponse.k");
      if ( currentResponse.k == k )
        return true;
    }
  }
  return false;
}

/****************************************************************
**
**
*****************************************************************/

const char * DreamFocuser::getDefaultName()
{
  return (char *)"DreamFocuser";
}
