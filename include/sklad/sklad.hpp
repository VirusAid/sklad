// sklad — an embedded LSM-tree key-value storage engine for C++17.
// SPDX-License-Identifier: MIT
//
// sklad.hpp — the single umbrella header.
//
//   #include <sklad/sklad.hpp>
//   using namespace sklad;
//
//   std::unique_ptr<db> store;
//   db::open({}, "mydb", &store);
//   store->put("key", "value");
//   std::string v;
//   store->get("key", &v);
#ifndef SKLAD_SKLAD_HPP
#define SKLAD_SKLAD_HPP

#include "sklad/status.hpp"
#include "sklad/options.hpp"
#include "sklad/write_batch.hpp"
#include "sklad/iterator.hpp"
#include "sklad/db.hpp"

#define SKLAD_VERSION_MAJOR 0
#define SKLAD_VERSION_MINOR 1
#define SKLAD_VERSION_PATCH 0
#define SKLAD_VERSION_STRING "0.1.0"

#endif  // SKLAD_SKLAD_HPP
