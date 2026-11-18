# Host build of tools/render.c + the plugin + LVGL from the FreeToon tree.
# Used by `make preview`; objects go to preview/build (not committed).
FREETOON_SRC ?= ../../../freetoon-lvgl
UI_SRC  := $(FREETOON_SRC)/lvgl_ui_recovered/src
OUT     := preview/build
HOSTCC  ?= cc
CFLAGS  := -O1 -g -w -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DMB_HOST_PREVIEW \
           -I. -I$(UI_SRC) -I$(UI_SRC)/lvgl

LVGL_SRC := $(shell find $(UI_SRC)/lvgl/src -name '*.c')
FONT_SRC := $(UI_SRC)/lv_font_montserrat_64_custom.c $(UI_SRC)/lv_font_montserrat_76_bold.c \
            $(UI_SRC)/lv_font_montserrat_96_custom.c $(wildcard $(UI_SRC)/lv_font_os_*.c)
LIB_OBJ  := $(patsubst $(UI_SRC)/%.c,$(OUT)/ft/%.o,$(LVGL_SRC) $(FONT_SRC))
# theme.c (FreeToon 1.1+) supplies the real palettes; older trees have none.
THEME_C  := $(wildcard $(UI_SRC)/theme.c)
APP_OBJ  := $(OUT)/screen_matter.o $(OUT)/matter_logic.o $(OUT)/matter_json.o $(OUT)/render.o \
            $(if $(THEME_C),$(OUT)/theme.o)
ifneq ($(THEME_C),)
CFLAGS   += -DMB_HAVE_THEMES
endif

$(OUT)/render: $(LIB_OBJ) $(APP_OBJ)
	$(HOSTCC) $^ -o $@ -lm

$(OUT)/ft/%.o: $(UI_SRC)/%.c
	@mkdir -p $(dir $@)
	@$(HOSTCC) $(CFLAGS) -c $< -o $@

$(OUT)/screen_matter.o: screen_matter.c matter_logic.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CFLAGS) -Wall -Wextra -Wno-unused-parameter -c $< -o $@
$(OUT)/matter_logic.o: matter_logic.c matter_logic.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CFLAGS) -c $< -o $@
$(OUT)/matter_json.o: matter_json.c matter_json.h
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CFLAGS) -c $< -o $@
$(OUT)/render.o: tools/render.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CFLAGS) -c $< -o $@
$(OUT)/theme.o: $(THEME_C)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(CFLAGS) -DFT_HOST -c $< -o $@
