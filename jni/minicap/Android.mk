LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := minicap-common

LOCAL_SRC_FILES := \
	JpgEncoder.cpp \
	SimpleServer.cpp \
	minicap.cpp \

LOCAL_STATIC_LIBRARIES := \
	libjpeg-turbo \

LOCAL_SHARED_LIBRARIES := \
	minicap-shared \

include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

# Enable PIE manually. Will get reset on $(CLEAR_VARS).
LOCAL_CFLAGS += -fPIE
LOCAL_LDFLAGS += -fPIE -pie

LOCAL_MODULE := minicap

LOCAL_STATIC_LIBRARIES := minicap-common

LOCAL_LDLIBS := -llog -landroid -lmediandk  

# -lEGL -lGLESv3

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_MODULE := minicap-nopie

LOCAL_STATIC_LIBRARIES := minicap-common

LOCAL_LDLIBS := -llog -landroid -lmediandk  

include $(BUILD_EXECUTABLE)




# /////////////////////////////////////////////////





# LOCAL_PATH := $(call my-dir)

# include $(CLEAR_VARS)

# LOCAL_MODULE := minicap-common

# LOCAL_SRC_FILES := \
#     JpgEncoder.cpp \
#     SimpleServer.cpp \
#     minicap.cpp \
#     libyuv/source/convert.cc \
#     libyuv/source/convert_argb.cc \
#     libyuv/source/convert_from.cc \
#     libyuv/source/convert_from_argb.cc \
#     libyuv/source/convert_to_i420.cc \
#     libyuv/source/planar_functions.cc \
#     libyuv/source/rotate.cc \
#     libyuv/source/row_common.cc \
#     libyuv/source/row_neon.cc \
#     libyuv/source/row_neon64.cc \
#     libyuv/source/row_any.cc \
#     libyuv/source/scale.cc \
#     libyuv/source/scale_neon.cc \
#     libyuv/source/scale_neon64.cc

# LOCAL_C_INCLUDES := \
#     $(LOCAL_PATH)/libyuv/include \
#     $(LOCAL_PATH)   # <-- add this so Minicap.hpp is visible

# LOCAL_CPPFLAGS += -O3 -fPIC
# LOCAL_CFLAGS   += -O3

# LOCAL_STATIC_LIBRARIES := libjpeg-turbo

# include $(BUILD_STATIC_LIBRARY)


# ##########################################################
# # Build minicap executable (statically link minicap-common)
# ##########################################################
# include $(CLEAR_VARS)

# LOCAL_MODULE := minicap

# LOCAL_STATIC_LIBRARIES := minicap-common

# LOCAL_LDLIBS := -llog -landroid -lmediandk

# # Enable PIE for the executable
# LOCAL_CFLAGS += -fPIE
# LOCAL_LDFLAGS += -fPIE -pie

# include $(BUILD_EXECUTABLE)
