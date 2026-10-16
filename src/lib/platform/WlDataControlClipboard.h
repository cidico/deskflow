/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QtGlobal>

#include <functional>
#include <memory>

namespace deskflow {

class EiClipboard;

//! Clipboard backend for compositors without the portal clipboard interface
/*!
Talks ext-data-control-v1 on its own Wayland connection and keeps an EiClipboard
cache in sync with the local selection. All Wayland calls run on one worker thread.
*/
class WlDataControlClipboard
{
public:
  using LocalChangeCallback = std::function<void()>;
  using MaxBytesCallback = std::function<qint64()>;

  WlDataControlClipboard(EiClipboard *cache, MaxBytesCallback maxBytes, LocalChangeCallback onLocalChange);
  ~WlDataControlClipboard();

  WlDataControlClipboard(const WlDataControlClipboard &) = delete;
  WlDataControlClipboard &operator=(const WlDataControlClipboard &) = delete;

  //! True while connected, bound to the protocol objects and the worker is running
  bool isAvailable() const;

  //! Snapshot the cache now and offer it as the local selection (thread-safe)
  void claimFromCache();

  struct Impl; // opaque, defined in the .cpp

private:
  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow
