///////////////////////////////////////////////////////////////////////////////
// FILE:          EvaNdeCtrl.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   EvaNdeCtrl adapter.  Needs accompanying firmware
// COPYRIGHT:     University of California, San Francisco, 2008
// LICENSE:       LGPL
// 
// AUTHOR:        Nico Stuurman, nico@cmp.ucsf.edu 11/09/2008
//                automatic device detection by Karl Hoover
//
//

#include "EvaNdeCtrl.h"
#include "XYStage.h"
#include "../../MMDevice/ModuleInterface.h"
#include <sstream>
#include <cstdio>
#include "SerialPort.h"

#ifdef WIN32
   #define WIN32_LEAN_AND_MEAN
   #include <windows.h>
   #define snprintf _snprintf 
#endif

const char* g_DeviceNameEvaNdeCtrlHub = "EvaNdeCtrl-Hub";
const char* g_DeviceNameEvaNdeCtrlXYStage = "XYStage";
//const char* g_DeviceNameEvaNdeCtrlZStage = "EvaNdeCtrl-ZStage";



// Global info about the state of the EvaNdeCtrl.  This should be folded into a class
unsigned g_switchState = 0;
unsigned g_shutterState = 0;
const int g_Min_MMVersion = 0;
const int g_Max_MMVersion = 10;
const char* g_versionProp = "Version";
const char* g_normalLogicString = "Normal";
const char* g_invertedLogicString = "Inverted";

// static lock
MMThreadLock CEvaNdeCtrlHub::lock_;

///////////////////////////////////////////////////////////////////////////////
// Exported MMDevice API
///////////////////////////////////////////////////////////////////////////////
MODULE_API void InitializeModuleData()
{
   AddAvailableDeviceName(g_DeviceNameEvaNdeCtrlHub, "Hub (required)");
   AddAvailableDeviceName(g_DeviceNameEvaNdeCtrlXYStage, "XYStage");
   //AddAvailableDeviceName(g_DeviceNameEvaNdeCtrlZStage, "ZStage");

}

MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
   if (deviceName == 0)
      return 0;

   if (strcmp(deviceName, g_DeviceNameEvaNdeCtrlHub) == 0)
   {
      return new CEvaNdeCtrlHub;
   }
   else if (strcmp(deviceName, g_DeviceNameEvaNdeCtrlXYStage) == 0)
   {
      return new XYStage;
   }
   //else if (strcmp(deviceName, g_DeviceNameEvaNdeCtrlZStage) == 0)
   //{
   //   return new CEvaNdeCtrlZStage;
   //}
 
   return 0;
}

MODULE_API void DeleteDevice(MM::Device* pDevice)
{
   delete pDevice;
}

///////////////////////////////////////////////////////////////////////////////
// CEvaNdeCtrlHUb implementation
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
//
CEvaNdeCtrlHub::CEvaNdeCtrlHub() :
initialized_ (false)
{
   portAvailable_ = false;

   timedOutputActive_ = false;

   InitializeDefaultErrorMessages();

   SetErrorText(ERR_PORT_OPEN_FAILED, "Failed opening EvaNdeCtrl USB device");
   SetErrorText(ERR_BOARD_NOT_FOUND, "Did not find an EvaNdeCtrl board with the correct firmware.  Is the EvaNdeCtrl board connected to this serial port?");
   SetErrorText(ERR_NO_PORT_SET, "Hub Device not found.  The EvaNdeCtrl Hub device is needed to create this device");
   std::ostringstream errorText;
   errorText << "The firmware version on the EvaNdeCtrl is not compatible with this adapter.  Please use firmware version ";
   errorText <<  g_Min_MMVersion << " to " << g_Max_MMVersion;
   SetErrorText(ERR_VERSION_MISMATCH, errorText.str().c_str());


   std::string selectPort="Undefined";

   CPropertyAction* pAct = new CPropertyAction(this, &CEvaNdeCtrlHub::OnPort);
   CreateProperty("ComPort", selectPort.c_str(), MM::String, false, pAct, true);

   //list all available ports
   SerialPortLister spl;
   std::vector<std::string> availablePorts;
   spl.ListPorts(availablePorts);


   std::vector<std::string>::iterator it;
   for (it=availablePorts.begin(); it!=availablePorts.end(); it++)
   {
	   selectPort = *it;
	   AddAllowedValue("ComPort",selectPort.c_str());
   }

   SetProperty("ComPort",selectPort.c_str());
}

CEvaNdeCtrlHub::~CEvaNdeCtrlHub()
{
   Shutdown();
   std::vector<SerialPort*>::iterator i;
   for (i=ports_.begin(); i!=ports_.end(); i++)
      delete *i;
}

void CEvaNdeCtrlHub::GetName(char* name) const
{
   CDeviceUtils::CopyLimitedString(name, g_DeviceNameEvaNdeCtrlHub);
}

bool CEvaNdeCtrlHub::Busy()
{
   return false;
}

// private and expects caller to:
// 1. guard the port
// 2. purge the port
int CEvaNdeCtrlHub::GetStatus()
{
   int ret = DEVICE_OK;
   std::string command ="?";
	
   comPort->Purge();
   ret = comPort->SetCommand(command.c_str(),"\n");
   if (ret != DEVICE_OK)
      return ret;
   unsigned char answer[30];
   unsigned long charsRead=0;

   char an[64];
   ret = comPort->GetAnswer(an,64,"\r\n");
   //ret = comPort->Read(answer,3,charsRead);
   if (ret != DEVICE_OK)
      return ret;

   //sample: <Idle,MPos:0.000,0.000,0.000,WPos:0.000,0.000,0.000>
   if (strlen(an) <1)
      return ERR_BOARD_NOT_FOUND;
   return ret;

}
int CEvaNdeCtrlHub::SendCommand(std::string command)
{
   int ret = DEVICE_OK;
	
   comPort->Purge();
   ret = comPort->SetCommand(command.c_str(),"\n");
   if (ret != DEVICE_OK)
      return ret;
   unsigned char answer[30];
   unsigned long charsRead=0;

   char an[64];
   ret = comPort->GetAnswer(an,64,"\r\n");
   //ret = comPort->Read(answer,3,charsRead);
   if (ret != DEVICE_OK)
      return ret;

   //sample: <Idle,MPos:0.000,0.000,0.000,WPos:0.000,0.000,0.000>
   if (strlen(an) <1)
      return DEVICE_ERR;
   return ret;

}
MM::DeviceDetectionStatus CEvaNdeCtrlHub::DetectDevice(void)
{
   if (initialized_)
      return MM::CanCommunicate;
   MM::DeviceDetectionStatus result = MM::CanNotCommunicate ;
   try
   {
	   if(DEVICE_OK == GetStatus())
	   result =  MM::CanCommunicate;
   }
   catch(...)
   {
      LogMessage("Exception in DetectDevice!",false);
   }

   return result;
}


int CEvaNdeCtrlHub::Initialize()
{

   // Name
   int ret = CreateProperty(MM::g_Keyword_Name, g_DeviceNameEvaNdeCtrlHub, MM::String, true);
   if (DEVICE_OK != ret)
      return ret;


   // The first second or so after opening the serial port, the EvaNdeCtrl is waiting for firmwareupgrades.  Simply sleep 1 second.
   CDeviceUtils::SleepMs(2000);

   MMThreadGuard myLock(lock_);

   //ret = SetProperty("ComPort","COM4");
   //if (DEVICE_OK != ret)
   //   return ret;

   // Check that we have a controller:
   ret = GetStatus();
   if( DEVICE_OK != ret)
      return ret;

   version_=8;
   if (version_ < g_Min_MMVersion || version_ > g_Max_MMVersion)
      return ERR_VERSION_MISMATCH;

   CPropertyAction* pAct = new CPropertyAction(this, &CEvaNdeCtrlHub::OnVersion);
   std::ostringstream sversion;
   sversion << version_;
   CreateProperty(g_versionProp, sversion.str().c_str(), MM::Integer, true, pAct);

   ret = UpdateStatus();
   if (ret != DEVICE_OK)
      return ret;

   // turn off verbose serial debug messages
   GetCoreCallback()->SetDeviceProperty(port_.c_str(), "Verbose", "0");
   initialized_ = true;
   return DEVICE_OK;
}

int CEvaNdeCtrlHub::DetectInstalledDevices()
{
   if (MM::CanCommunicate == DetectDevice()) 
   {
      std::vector<std::string> peripherals; 
      peripherals.clear();
      peripherals.push_back(g_DeviceNameEvaNdeCtrlXYStage);
      //peripherals.push_back(g_DeviceNameEvaNdeCtrlZStage);

      for (size_t i=0; i < peripherals.size(); i++) 
      {
         MM::Device* pDev = ::CreateDevice(peripherals[i].c_str());
         if (pDev) 
         {
            AddInstalledDevice(pDev);
         }
      }
   }

   return DEVICE_OK;
}



int CEvaNdeCtrlHub::Shutdown()
{
   initialized_ = false;
   DestroyPort(comPort);
   return DEVICE_OK;
}

int CEvaNdeCtrlHub::OnPort(MM::PropertyBase* pProp, MM::ActionType pAct)
{
   if (pAct == MM::BeforeGet)
   {
      pProp->Set(port_.c_str());
   }
   else if (pAct == MM::AfterSet)
   {
      pProp->Get(port_);
	  DestroyPort(comPort);
	  comPort = CreatePort(port_.c_str());
	  comPort->SetProperty("AnswerTimeout","0.4");
	  comPort->SetProperty(MM::g_Keyword_Handshaking, "Off");
      portAvailable_ = true;
   }
   return DEVICE_OK;
}

int CEvaNdeCtrlHub::OnVersion(MM::PropertyBase* pProp, MM::ActionType pAct)
{
   if (pAct == MM::BeforeGet)
   {
      pProp->Set((long)version_);
   }
   return DEVICE_OK;
}


SerialPort* CEvaNdeCtrlHub::CreatePort(const char* portName)
{
   // check if the port already exists
   std::vector<SerialPort*>::iterator i;
   for (i=ports_.begin(); i!=ports_.end(); i++)
   {
      char name[MM::MaxStrLength];
      (*i)->GetName(name);
      if (strcmp(name, portName) == 0)
      {
          (*i)->LogMessage(("adding reference to Port " + std::string(portName)).c_str() , true);
         (*i)->AddReference();
		 (*i)->Initialize();
         return *i;
      }
   }

   // no such port found, so try to create a new one
   SerialPort* pPort = new SerialPort(portName);
   pPort->Initialize();
   //pPort->LogMessage(("created new Port " + std::string(portName)).c_str() , true);
   ports_.push_back(pPort);
   pPort->AddReference();
   //pPort->LogMessage(("adding reference to Port " + std::string(portName)).c_str() , true);
   return pPort;

}

void CEvaNdeCtrlHub::DestroyPort(SerialPort* port)
{
   std::vector<SerialPort*>::iterator i;
   for (i=ports_.begin(); i!=ports_.end(); i++)
   {
      if (*i == port)
      {
         char theName[MM::MaxStrLength];
         (*i)->GetName(theName);
         //(*i)->LogMessage("Removing reference to Port " + std::string(theName) , true);
         (*i)->RemoveReference();

         // really destroy only if there are no references pointing to the port
         if ((*i)->OKToDelete())
         {
            //(*i)->LogMessage("deleting Port " + std::string(theName)) , true);
            delete *i;
            ports_.erase(i);
         }
         return;       
      }
   }
}

