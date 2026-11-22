#pragma once

#include <lvgl.h>
#include <cstdint>
#include <initializer_list>

namespace files {

class BottomKeyBar {
public:
    struct Item { uint32_t key; const void* image; };
    explicit BottomKeyBar(lv_obj_t* parent);
    ~BottomKeyBar();
    void setItems(std::initializer_list<Item> items);

private:
    lv_obj_t* _bar;
    lv_obj_t* _label;
};

}  // namespace files