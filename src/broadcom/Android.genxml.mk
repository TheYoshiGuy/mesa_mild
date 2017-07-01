# Copyright © 2016 Intel Corporation
# Copyright © 2016 Mauro Rossi <issor.oruam@gmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
#

include $(CLEAR_VARS)

LOCAL_MODULE := libmesa_broadcom_genxml

LOCAL_MODULE_CLASS := STATIC_LIBRARIES

intermediates := $(call local-generated-sources-dir)

# dummy.c source file is generated to meet the build system's rules.
LOCAL_GENERATED_SOURCES += $(intermediates)/dummy.c

$(intermediates)/dummy.c:
	@mkdir -p $(dir $@)
	@echo "Gen Dummy: $(PRIVATE_MODULE) <= $(notdir $(@))"
	$(hide) touch $@

# This is the list of auto-generated files headers
LOCAL_GENERATED_SOURCES += $(addprefix $(intermediates)/, $(BROADCOM_GENXML_GENERATED_FILES))

define header-gen
	@mkdir -p $(dir $@)
	@echo "Gen Header: $(PRIVATE_MODULE) <= $(notdir $(@))"
	$(hide) $(PRIVATE_SCRIPT) $(PRIVATE_SCRIPT_FLAGS) $(PRIVATE_XML) > $@
endef

$(intermediates)/cle/v3d_packet_v21_pack.h: PRIVATE_SCRIPT := $(MESA_PYTHON2) $(LOCAL_PATH)/cle/gen_pack_header.py
$(intermediates)/cle/v3d_packet_v21_pack.h: PRIVATE_XML := $(LOCAL_PATH)/cle/v3d_packet_v21.xml
$(intermediates)/cle/v3d_packet_v21_pack.h: $(LOCAL_PATH)/cle/v3d_packet_v21.xml $(LOCAL_PATH)/cle/gen_pack_header.py
	$(call header-gen)

LOCAL_EXPORT_C_INCLUDE_DIRS := \
	$(MESA_TOP)/src/broadcom \
	$(intermediates)

include $(MESA_COMMON_MK)
include $(BUILD_STATIC_LIBRARY)
