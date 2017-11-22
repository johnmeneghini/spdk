#
#  BSD LICENSE
#
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#    * Neither the name of Intel Corporation nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

CFLAGS += $(ENV_CFLAGS) -I$(SPDK_ROOT_DIR)/lib -I$(SPDK_ROOT_DIR)/app
LIBNAME = nvmf

C_SRCS = discovery.c subsystem.c nvmf.c \
	 request.c session.c transport.c \
	 direct.c virtual.c

C_SRCS-$(CONFIG_RDMA) += rdma.c

C_SRCS-$(CONFIG_BCM_FC) += \
    ./nvmf_fc/bcm_fc.c  \
    ./nvmf_fc/bcm_sli_fc.c  \
    ./nvmf_fc/nvmf_fc_ls.c  \
    ./nvmf_fc/nvmf_fc_poller_api.c 

DIRS-$(CONFIG_BCM_FC) += nvmf_fc

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk

# Disable clang warning: taking address of packed member of class or structure may result in an unaligned pointer value [-Werror,-Waddress-of-packed-member]
ifeq ($(CC),clang)
CLANG_VERSION := $(shell $(CC) -v 2>&1 | \
	sed -n "s/.*version \([0-9]*\.[0-9]*\).*/\1/p")

CLANG_MAJOR_VERSION := $(shell echo $(CLANG_VERSION) | cut -f1 -d.)

ifeq ($(shell test $(CLANG_MAJOR_VERSION) -ge 4 && echo 1), 1)
CFLAGS += -Wno-address-of-packed-member
endif
endif
