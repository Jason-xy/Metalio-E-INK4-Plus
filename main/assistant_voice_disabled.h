#pragma once

#if CONFIG_INKMETER_ENABLED

#include "assistant_screen/assistant_screen.h"
#include "workbuddy_screen/workbuddy_screen.h"

// The InkMeter profile keeps shared callers while omitting both voice apps.
inline bool AssistantScreen::IsActive() { return false; }
inline bool AssistantScreen::IsPttHeld() { return false; }
inline bool AssistantScreen::IsPttWaveVisible() { return false; }
inline void AssistantScreen::SyncPttOverlay() {}
inline void AssistantScreen::SetStandby(bool) {}
inline void AssistantScreen::RequestOpen() {}
inline void AssistantScreen::AddMessage(const char*, const char*) {}
inline void AssistantScreen::SetEmotion(const char*) {}
inline void AssistantScreen::AddYunxinSpeech(const char*) {}
inline void AssistantScreen::FinishYunxinSpeech() {}
inline void AssistantScreen::FlushYunxinSpeech() {}
inline bool WorkBuddyScreen::IsPttWaveVisible() { return false; }
inline void WorkBuddyScreen::SetStandby(bool) {}

#endif
