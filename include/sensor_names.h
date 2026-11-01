#ifndef FIRECONTROL_SENSOR_NAMES_H
#define FIRECONTROL_SENSOR_NAMES_H

#include "json_status.h"  // kInfoSourceCount

namespace firecontrol
{
    // 信息源的显示名。首页按钮、接口页标题、控制台日志都用这一份, 避免三处各写各的。
    //
    // 名字由用户定: 第一路现场接的是"一号传感器", 其余先保留"信息源 N";
    // 以后接一个改一个, 只改这张表。
    inline constexpr const char *kSensorNames[kInfoSourceCount] = {
        "一号传感器",
        "信息源 2", "信息源 3", "信息源 4",
        "信息源 5", "信息源 6", "信息源 7",
        "信息源 8", "信息源 9", "信息源 10",
        "信息源 11", "信息源 12",
    };
}

#endif // FIRECONTROL_SENSOR_NAMES_H
