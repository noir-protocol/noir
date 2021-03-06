// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <noir/common/helper/variant.h> // FIXME: remove later
#include <fc/log/logger_config.hpp>

namespace noir::log {

extern const char* default_logger_name;

void initialize(const char* logger_name);

#define FC_DEBUG_LOG(LOGGER_NAME, FORMAT, ...) fc_dlog(fc::logger::get(LOGGER_NAME), FORMAT, __VA_ARGS__)
#define FC_INFO_LOG(LOGGER_NAME, FORMAT, ...) fc_ilog(fc::logger::get(LOGGER_NAME), FORMAT, __VA_ARGS__)
#define FC_WARN_LOG(LOGGER_NAME, FORMAT, ...) fc_wlog(fc::logger::get(LOGGER_NAME), FORMAT, __VA_ARGS__)
#define FC_ERROR_LOG(LOGGER_NAME, FORMAT, ...) fc_elog(fc::logger::get(LOGGER_NAME), FORMAT, __VA_ARGS__)

#ifdef dlog
#  undef dlog
#  define dlog(FORMAT, ...) FC_DEBUG_LOG(noir::log::default_logger_name, FORMAT, __VA_ARGS__)
#endif

#ifdef ilog
#  undef ilog
#  define ilog(FORMAT, ...) FC_INFO_LOG(noir::log::default_logger_name, FORMAT, __VA_ARGS__)
#endif

#ifdef wlog
#  undef wlog
#  define wlog(FORMAT, ...) FC_WARN_LOG(noir::log::default_logger_name, FORMAT, __VA_ARGS__)
#endif

#ifdef elog
#  undef elog
#  define elog(FORMAT, ...) FC_ERROR_LOG(noir::log::default_logger_name, FORMAT, __VA_ARGS__)
#endif

} // namespace noir::log
