/* SPDX-License-Identifier: Apache-2.0
 * The two lengths cutils/properties.h takes from bionic, for a host test that
 * compiles this device's gralloc against the AOSP headers and the host's libc
 * (gralloc/tests/recover_test.cpp). Bionic's own header cannot be used there:
 * it is the C library's, and this test links against the host's.
 *
 * The values are bionic's (bionic/libc/include/sys/system_properties.h). They
 * are the sizes of the buffers property_get() fills, and this test's own
 * property_get() fills none of them, so nothing here depends on the numbers
 * being right -- only on their being what the device is compiled with. */
#pragma once

#define PROP_NAME_MAX 32
#define PROP_VALUE_MAX 92
