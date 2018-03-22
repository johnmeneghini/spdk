#
#  BSD LICENSE
#
#  Copyright (c) 2017 Broadcom.  All Rights Reserved.
#  The term "Broadcom" refers to Broadcom Limited and/or its subsidiaries.
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

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../../)
SPDK_LIB_NVMF_FC_DIR = $(SPDK_ROOT_DIR)/lib/nvmf/nvmf_fc

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

CFLAGS += -I$(SPDK_ROOT_DIR)/lib -I$(SPDK_LIB_NVMF_FC_DIR) -I$(SPDK_LIB_NVMF_DIR)

TEST_FILE = nvmf_fc_ls_ut.c
OTHER_FILES = nvmf_fc_ls.o \
	nvmf_fc_poller_api.o \
	session.c \

include $(SPDK_ROOT_DIR)/mk/nvmf.unittest.mk

#Rules for modules in nvmf_fc folder under nvmf folder
#in order to compile the .o file into local directory.
nvmf_fc_ls.o : $(SPDK_LIB_NVMF_FC_DIR)/nvmf_fc_ls.c
	$(COMPILE_C)

nvmf_fc_poller_api.o : $(SPDK_LIB_NVMF_FC_DIR)/nvmf_fc_poller_api.c
	$(COMPILE_C)
