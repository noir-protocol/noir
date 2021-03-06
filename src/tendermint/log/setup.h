// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <tendermint/log/log.h>

namespace tendermint::log {

void setup(Logger* logger = nullptr);

} // namespace tendermint::log
