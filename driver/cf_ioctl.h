// cf_ioctl.h — shared IOCTL codes between the kernel driver and
// user-mode. Include from both sides so values can never drift.

#pragma once

// Mirror the kernel CTL_CODE macro so user-mode compiles too.
// CTL_CODE(DeviceType, Function, Method, Access)
//   = (DeviceType << 16) | (Access << 14) | (Function << 2) | Method
#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif

#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x22
#endif
#ifndef METHOD_NEITHER
#define METHOD_NEITHER      3
#endif
#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS     0
#endif

#define CF_ARM         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1000, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_READ        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1001, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_WRITE       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1002, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_BATCH       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1003, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_INVOKE      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1004, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_ALLOC_RWX   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1008, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_FREE_RWX    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x1009, METHOD_NEITHER, FILE_ANY_ACCESS)
#define CF_PATCH_QWORD CTL_CODE(FILE_DEVICE_UNKNOWN, 0x100A, METHOD_NEITHER, FILE_ANY_ACCESS)
