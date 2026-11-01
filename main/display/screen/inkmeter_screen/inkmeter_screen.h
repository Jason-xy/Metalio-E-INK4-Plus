#pragma once
#include <lvgl.h>

class InkMeterScreen {
  public:
    static lv_obj_t *Create();
    static bool IsActive();
    static void SetStandby(bool standby);
};
