/*
 *      Copyright (C) 2014 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "input/ButtonMapper.h"
#include "input/InputManager.h"
#include "libretro/ClientBridge.h"
#include "libretro/libretro.h"
#include "libretro/LibretroDLL.h"
#include "libretro/LibretroEnvironment.h"
#include "log/Log.h"
#include "log/LogAddon.h"
#include "settings/Settings.h"
#include "GameInfoLoader.h"

#include "kodi/libXBMC_addon.h"
#include "kodi/libKODI_game.h"
#include "kodi/xbmc_addon_dll.h"
#include "kodi/kodi_game_dll.h"

#include <set>
#include <string>
#include <vector>

using namespace ADDON;
using namespace LIBRETRO;

#define GAME_CLIENT_NAME_UNKNOWN      "Unknown libretro core"
#define GAME_CLIENT_VERSION_UNKNOWN   "0.0.0"

#ifndef SAFE_DELETE
#define SAFE_DELETE(x)  do { delete x; x = NULL; } while (0)
#endif

void SAFE_DELETE_GAME_INFO(std::vector<CGameInfoLoader*>& vec)
{
  for (std::vector<CGameInfoLoader*>::iterator it = vec.begin(); it != vec.end(); ++it)
    delete *it;
  vec.clear();
}

namespace LIBRETRO
{
  CHelper_libXBMC_addon*        XBMC          = NULL;
  CHelper_libKODI_game*         FRONTEND      = NULL;
  CLibretroDLL*                 CLIENT        = NULL;
  CClientBridge*                CLIENT_BRIDGE = NULL;
  std::vector<CGameInfoLoader*> GAME_INFO;
  bool                          SUPPORTS_VFS = false; // TODO
}

extern "C"
{

ADDON_STATUS ADDON_Create(void* callbacks, void* props)
{
  try
  {
    if (!callbacks || !props)
      throw ADDON_STATUS_UNKNOWN;

    game_client_properties* gameClientProps = static_cast<game_client_properties*>(props);

    if (gameClientProps->game_client_dll_path == NULL)
      throw ADDON_STATUS_UNKNOWN;

    XBMC = new CHelper_libXBMC_addon;
    if (!XBMC || !XBMC->RegisterMe(callbacks))
      throw ADDON_STATUS_PERMANENT_FAILURE;

    CLog::Get().SetPipe(new CLogAddon(XBMC));

    FRONTEND = new CHelper_libKODI_game;
    if (!FRONTEND || !FRONTEND->RegisterMe(callbacks))
      throw ADDON_STATUS_PERMANENT_FAILURE;

    CLIENT = new CLibretroDLL(XBMC);
    if (!CLIENT->Load(gameClientProps))
    {
      XBMC->Log(LOG_ERROR, "Failed to load %s", gameClientProps->game_client_dll_path);
      throw ADDON_STATUS_PERMANENT_FAILURE;
    }

    unsigned int version = CLIENT->retro_api_version();
    if (version != 1)
    {
      XBMC->Log(LOG_ERROR, "Expected libretro api v1, found version %u", version);
      throw ADDON_STATUS_PERMANENT_FAILURE;
    }

    // Environment must be initialized before calling retro_init()
    CLIENT_BRIDGE = new CClientBridge;
    CLibretroEnvironment::Get().Initialize(XBMC, FRONTEND, CLIENT, CLIENT_BRIDGE);

    CButtonMapper::Get().LoadButtonMap();

    CLIENT->retro_init();

    // Log core info
    retro_system_info systemInfo = { };
    CLIENT->retro_get_system_info(&systemInfo);

    // VFS support is derived from need_fullpath. This property means that the
    // libretro cores requires a valid pathname. Conversely, if need_fullpath
    // is false, the core can load from memory.
    SUPPORTS_VFS = !systemInfo.need_fullpath;

    std::string libraryName = systemInfo.library_name ? systemInfo.library_name : "";
    std::string libraryVersion = systemInfo.library_version ? systemInfo.library_version : "";
    std::string extensions = systemInfo.valid_extensions ? systemInfo.valid_extensions : "";

    XBMC->Log(LOG_DEBUG, "CORE: ----------------------------------");
    XBMC->Log(LOG_DEBUG, "CORE: Library name:    %s", libraryName.c_str());
    XBMC->Log(LOG_DEBUG, "CORE: Library version: %s", libraryVersion.c_str());
    XBMC->Log(LOG_DEBUG, "CORE: Extensions:      %s", extensions.c_str());
    XBMC->Log(LOG_DEBUG, "CORE: Supports VFS:    %s", SUPPORTS_VFS ? "true" : "false");
    XBMC->Log(LOG_DEBUG, "CORE: ----------------------------------");

    // Reject invalid properties
    std::set<std::string> coreExtensions; // TODO: Parse string from libretro API
    std::set<std::string> addonExtensions; // TODO: Convert char** to set<string>

    if (coreExtensions != addonExtensions)
    {
      std::string strAddonExtensions;// = StringUtils::Join(addonExtensions, "|"); // TODO
      XBMC->Log(LOG_ERROR, "CORE: Extensions don't match addon.xml: %s", strAddonExtensions.c_str());
      throw ADDON_STATUS_PERMANENT_FAILURE;
    }

    if (gameClientProps->supports_vfs != SUPPORTS_VFS)
    {
      XBMC->Log(LOG_ERROR, "CORE: VFS support doesn't match addon.xml: %s", gameClientProps->supports_vfs ? "true" : "false");
      throw ADDON_STATUS_PERMANENT_FAILURE;
    }
  }
  catch (const ADDON_STATUS& status)
  {
    SAFE_DELETE(XBMC);
    SAFE_DELETE(FRONTEND);
    SAFE_DELETE(CLIENT);
    SAFE_DELETE(CLIENT_BRIDGE);
    return status;
  }

  return ADDON_GetStatus();
}

void ADDON_Stop(void)
{
}

void ADDON_Destroy(void)
{
  if (CLIENT)
    CLIENT->retro_deinit();

  CLibretroEnvironment::Get().Deinitialize();

  CLog::Get().SetType(SYS_LOG_TYPE_CONSOLE);

  SAFE_DELETE(XBMC);
  SAFE_DELETE(FRONTEND);
  SAFE_DELETE(CLIENT);
  SAFE_DELETE(CLIENT_BRIDGE);
  SAFE_DELETE_GAME_INFO(GAME_INFO);
}

ADDON_STATUS ADDON_GetStatus(void)
{
  if (!XBMC || !FRONTEND || !CLIENT || !CLIENT_BRIDGE)
    return ADDON_STATUS_UNKNOWN;

  if (!CSettings::Get().IsInitialized())
    return ADDON_STATUS_NEED_SETTINGS;

  return ADDON_STATUS_OK;
}

bool ADDON_HasSettings(void)
{
  return false;
}

unsigned int ADDON_GetSettings(ADDON_StructSetting*** sSet)
{
  return 0;
}

ADDON_STATUS ADDON_SetSetting(const char* settingName, const void* settingValue)
{
  if (!settingName || !settingValue)
    return ADDON_STATUS_UNKNOWN;

  CSettings::Get().SetSetting(settingName, settingValue);
  CLibretroEnvironment::Get().SetSetting(settingName, static_cast<const char*>(settingValue));

  return ADDON_STATUS_OK;
}

void ADDON_FreeSettings(void)
{
}

void ADDON_Announce(const char* flag, const char* sender, const char* message, const void* data)
{
}

const char* GetGameAPIVersion(void)
{
  return GAME_API_VERSION;
}

const char* GetMininumGameAPIVersion(void)
{
  return GAME_MIN_API_VERSION;
}

GAME_ERROR LoadGame(const char* url)
{
  if (!CLIENT)
    return GAME_ERROR_FAILED;

  if (url == NULL)
    return GAME_ERROR_INVALID_PARAMETERS;

  // Build info loader vector
  SAFE_DELETE_GAME_INFO(GAME_INFO);
  GAME_INFO.push_back(new CGameInfoLoader(url, XBMC, SUPPORTS_VFS));

  bool bResult = false;

  // Try to load via memory
  retro_game_info gameInfo;
  if (GAME_INFO[0]->Load())
  {
    GAME_INFO[0]->GetMemoryStruct(gameInfo);
    bResult = CLIENT->retro_load_game(&gameInfo);
  }

  if (!bResult)
  {
    // Fall back to loading via path
    GAME_INFO[0]->GetPathStruct(gameInfo);
    bResult = CLIENT->retro_load_game(&gameInfo);
  }

  if (bResult)
    CInputManager::Get().OpenPort(0);

  return bResult ? GAME_ERROR_NO_ERROR : GAME_ERROR_FAILED;
}

GAME_ERROR LoadGameSpecial(SPECIAL_GAME_TYPE type, const char** urls, size_t urlCount)
{
  if (!CLIENT)
    return GAME_ERROR_FAILED;

  if (urls == NULL || urlCount == 0)
    return GAME_ERROR_INVALID_PARAMETERS;

  // TODO
  return GAME_ERROR_FAILED;
  /*
  retro_system_info info = { };
  CLIENT->retro_get_system_info(&info);
  const bool bSupportsVFS = !info.need_fullpath;

  // Build info loader vector
  SAFE_DELETE_GAME_INFO(GAME_INFO);
  for (unsigned int i = 0; i < urlCount; i++)
    GAME_INFO.push_back(new CGameInfoLoader(urls[i], XBMC, bSupportsVFS));

  // Try to load via memory
  std::vector<retro_game_info> infoVec;
  infoVec.resize(urlCount);
  bool bLoadFromMemory = true;
  for (unsigned int i = 0; bLoadFromMemory && i < urlCount; i++)
    bLoadFromMemory &= GAME_INFO[i]->GetMemoryStruct(infoVec[i]);
  if (bLoadFromMemory)
  {
    if (CLIENT->retro_load_game_special(type, infoVec.data(), urlCount))
      return GAME_ERROR_NO_ERROR;
  }

  // Fall back to loading by path
  for (unsigned int i = 0; i < urlCount; i++)
    GAME_INFO[i]->GetPathStruct(infoVec[i]);
  bool result = CLIENT->retro_load_game_special(type, infoVec.data(), urlCount);

  return result ? GAME_ERROR_NO_ERROR : GAME_ERROR_FAILED;
  */
}

GAME_ERROR LoadStandalone(void)
{
  if (!CLIENT)
    return GAME_ERROR_FAILED;

  retro_game_info empty = { "", NULL, 0, NULL };
  if (!CLIENT->retro_load_game(&empty))
    return GAME_ERROR_FAILED;

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR UnloadGame(void)
{
  GAME_ERROR error = GAME_ERROR_FAILED;

  if (CLIENT)
  {
    CLIENT->retro_unload_game();

    CInputManager::Get().ClosePorts();

    error = GAME_ERROR_NO_ERROR;
  }

  SAFE_DELETE_GAME_INFO(GAME_INFO);

  return error;
}

GAME_ERROR GetGameInfo(game_system_av_info* info)
{
  if (!CLIENT)
    return GAME_ERROR_FAILED;

  if (info == NULL)
    return GAME_ERROR_INVALID_PARAMETERS;

  retro_system_av_info retro_info = { };
  CLIENT->retro_get_system_av_info(&retro_info);

  info->geometry.base_width   = retro_info.geometry.base_width;
  info->geometry.base_height  = retro_info.geometry.base_height;
  info->geometry.max_width    = retro_info.geometry.max_width;
  info->geometry.max_height   = retro_info.geometry.max_height;
  info->geometry.aspect_ratio = retro_info.geometry.aspect_ratio;
  info->timing.fps            = retro_info.timing.fps;
  info->timing.sample_rate    = retro_info.timing.sample_rate;

  // Report info to CLibretroEnvironment
  CLibretroEnvironment::Get().UpdateSystemInfo(*info);

  return GAME_ERROR_NO_ERROR;
}

GAME_REGION GetRegion(void)
{
  if (!CLIENT)
    return GAME_REGION_UNKNOWN;

  return CLIENT->retro_get_region() == RETRO_REGION_NTSC ? GAME_REGION_NTSC : GAME_REGION_PAL;
}

GAME_ERROR RunFrame(void)
{
  if (!CLIENT)
    return GAME_ERROR_FAILED;

  CLIENT->retro_run();

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR Reset(void)
{
  if (!CLIENT)
    return GAME_ERROR_FAILED;

  CLIENT->retro_reset();

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR HwContextReset()
{
  if (!CLIENT_BRIDGE)
    return GAME_ERROR_FAILED;

  return CLIENT_BRIDGE->HwContextReset();
}

GAME_ERROR HwContextDestroy()
{
  if (!CLIENT_BRIDGE)
    return GAME_ERROR_FAILED;

  return CLIENT_BRIDGE->HwContextDestroy();
}

void UpdatePort(unsigned int port, bool connected, const game_controller* controller)
{
  if (connected)
  {
    if (!controller || !controller->controller_id)
      return;
  }

  CInputManager::Get().DeviceConnected(port, connected, connected ? controller : NULL);

  const unsigned int device = CInputManager::Get().GetDevice(port);

  if (CLIENT)
    CLIENT->retro_set_controller_port_device(port, device);
}

bool HasFeature(const char* controller_id, const char* feature_name)
{
  if (controller_id == nullptr || feature_name == nullptr)
    return false;

  return CButtonMapper::Get().GetLibretroIndex(controller_id, feature_name) >= 0;
}

bool InputEvent(const game_input_event* event)
{
  if (!event)
    return false;

  return CInputManager::Get().InputEvent(*event);
}

size_t SerializeSize(void)
{
  if (!CLIENT)
    return 0;

  return CLIENT->retro_serialize_size();
}

GAME_ERROR Serialize(uint8_t* data, size_t size)
{
  if (!CLIENT)
    return GAME_ERROR_FAILED;

  if (data == NULL)
    return GAME_ERROR_INVALID_PARAMETERS;

  bool result = CLIENT->retro_serialize(data, size);

  return result ? GAME_ERROR_NO_ERROR : GAME_ERROR_FAILED;
}

GAME_ERROR Deserialize(const uint8_t* data, size_t size)
{
  if (!CLIENT)
    return GAME_ERROR_FAILED;

  if (data == NULL)
    return GAME_ERROR_INVALID_PARAMETERS;

  bool result = CLIENT->retro_unserialize(data, size);

  return result ? GAME_ERROR_NO_ERROR : GAME_ERROR_FAILED;
}

GAME_ERROR CheatReset(void)
{
  if (!CLIENT)
    return GAME_ERROR_FAILED;

  CLIENT->retro_cheat_reset();

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR GetMemory(GAME_MEMORY type, const uint8_t** data, size_t* size)
{
  if (!CLIENT)
    return GAME_ERROR_FAILED;

  if (data == NULL || size == NULL)
    return GAME_ERROR_INVALID_PARAMETERS;

  *data = static_cast<const uint8_t*>(CLIENT->retro_get_memory_data(type));
  *size = CLIENT->retro_get_memory_size(type);

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR SetCheat(unsigned int index, bool enabled, const char* code)
{
  if (!CLIENT)
    return GAME_ERROR_FAILED;

  CLIENT->retro_cheat_set(index, enabled, code);

  return GAME_ERROR_NO_ERROR;
}

} // extern "C"
