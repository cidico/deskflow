/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/WlDataControlClipboard.h"

#include "base/Log.h"
#include "deskflow/IClipboard.h"
#include "platform/EiClipboard.h"
#include "platform/PortalClipboard.h"

#include "ext-data-control-v1-client-protocol.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <wayland-client.h>

namespace deskflow {

namespace {

constexpr const char *kOwnedMarker = "application/x-deskflow-owned";
constexpr const char *kPngMime = "image/png";
constexpr const char *kTextMimes[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"};
constexpr const char *kOfferedTextMimes[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"};

constexpr qint64 kMaxReadBytes = 256LL * 1024 * 1024;
constexpr int kReadWaitMs = 500;
constexpr int kReadDeadlineMs = 3000;
constexpr int kWriteWaitMs = 2000;
constexpr size_t kChunkBytes = 64 * 1024;

// A broken reader must not kill the process with SIGPIPE
void blockSigpipe()
{
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

struct Payload
{
  QByteArray text;
  QByteArray png;
};

QByteArray computeDigest(const QByteArray &text, const QByteArray &dib)
{
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(QByteArrayView("T"));
  hash.addData(QByteArrayView(QByteArray::number(text.size())));
  hash.addData(text);
  hash.addData(QByteArrayView("B"));
  hash.addData(QByteArrayView(QByteArray::number(dib.size())));
  hash.addData(dib);
  return hash.result();
}

// Tracks the detached writer threads so the owner can wait for them on shutdown
struct WriterGate
{
  std::atomic<int> active{0};
  std::atomic<bool> stop{false};
};

void writeToFd(int fd, const std::shared_ptr<const QByteArray> &data, const std::shared_ptr<WriterGate> &gate)
{
  blockSigpipe();
  const int flags = fcntl(fd, F_GETFL);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  const char *buf = data->constData();
  const size_t total = static_cast<size_t>(data->size());
  size_t written = 0;
  while (written < total && !gate->stop) {
    pollfd pfd{fd, POLLOUT, 0};
    const int rc = poll(&pfd, 1, kWriteWaitMs);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (rc == 0) {
      LOG_WARN("clipboard: reader stalled, giving up sending %zu bytes", total);
      break;
    }
    if (!(pfd.revents & POLLOUT))
      break; // reader went away

    const ssize_t n = write(fd, buf + written, std::min(kChunkBytes, total - written));
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      break; // EPIPE or a real error
    }
    written += static_cast<size_t>(n);
  }
  close(fd);
  if (written == total)
    LOG_DEBUG("clipboard: sent %zu bytes to the wayland reader", written);
  else
    LOG_DEBUG("clipboard: send aborted after %zu of %zu bytes", written, total);
}

} // namespace

struct WlDataControlClipboard::Impl
{
  EiClipboard *cache = nullptr;
  MaxBytesCallback maxBytes;
  LocalChangeCallback onLocalChange;

  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  wl_seat *seat = nullptr;
  ext_data_control_manager_v1 *manager = nullptr;
  ext_data_control_device_v1 *device = nullptr;
  int wakeFd = -1;

  std::thread worker;
  std::atomic<bool> available{false};
  std::atomic<bool> stop{false};
  std::shared_ptr<WriterGate> gate = std::make_shared<WriterGate>();

  // Guards pendingClaim and lastDigest
  std::mutex mutex;
  std::shared_ptr<const Payload> pendingClaim;
  QByteArray lastDigest;
  // Serialises our own access to the cache (the cache itself is not re-entrant)
  std::mutex cacheMutex;

  // Worker thread state
  std::map<ext_data_control_offer_v1 *, std::vector<std::string>> offers;
  ext_data_control_offer_v1 *selection = nullptr;
  ext_data_control_offer_v1 *primary = nullptr;
  bool selectionDirty = false;
  bool finished = false;
  ext_data_control_source_v1 *source = nullptr;
  std::shared_ptr<const Payload> sourcePayload;

  bool connect();
  void teardown();
  void run();
  void wake() const;
  void applyClaim(const std::shared_ptr<const Payload> &payload);
  void destroySource();
  void destroyOffer(ext_data_control_offer_v1 *&offer);
  bool offered(const ext_data_control_offer_v1 *offer, const char *mime) const;
  bool flush();
  QByteArray readOffer(ext_data_control_offer_v1 *offer, const char *mime, qint64 limit);
  void readSelection();

  static void onGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
  static void onGlobalRemove(void *, wl_registry *, uint32_t)
  {
  }
  static void onDataOffer(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer);
  static void onSelection(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer);
  static void onFinished(void *data, ext_data_control_device_v1 *);
  static void onPrimarySelection(void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer);
  static void onOfferMime(void *data, ext_data_control_offer_v1 *offer, const char *mime);
  static void onSend(void *data, ext_data_control_source_v1 *source, const char *mime, int32_t fd);
  static void onCancelled(void *data, ext_data_control_source_v1 *source);
};

static const wl_registry_listener kRegistryListener = {
    WlDataControlClipboard::Impl::onGlobal, WlDataControlClipboard::Impl::onGlobalRemove
};
static const ext_data_control_device_v1_listener kDeviceListener = {
    WlDataControlClipboard::Impl::onDataOffer, WlDataControlClipboard::Impl::onSelection,
    WlDataControlClipboard::Impl::onFinished, WlDataControlClipboard::Impl::onPrimarySelection
};
static const ext_data_control_offer_v1_listener kOfferListener = {WlDataControlClipboard::Impl::onOfferMime};
static const ext_data_control_source_v1_listener kSourceListener = {
    WlDataControlClipboard::Impl::onSend, WlDataControlClipboard::Impl::onCancelled
};

void WlDataControlClipboard::Impl::onGlobal(
    void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version
)
{
  auto *self = static_cast<Impl *>(data);
  if (!self->seat && std::strcmp(interface, wl_seat_interface.name) == 0) {
    self->seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
  } else if (!self->manager && std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
    Q_UNUSED(version);
    self->manager = static_cast<ext_data_control_manager_v1 *>(
        wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1)
    );
  }
}

void WlDataControlClipboard::Impl::onDataOffer(
    void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer
)
{
  auto *self = static_cast<Impl *>(data);
  self->offers[offer];
  ext_data_control_offer_v1_add_listener(offer, &kOfferListener, self);
}

void WlDataControlClipboard::Impl::onOfferMime(void *data, ext_data_control_offer_v1 *offer, const char *mime)
{
  static_cast<Impl *>(data)->offers[offer].emplace_back(mime);
}

void WlDataControlClipboard::Impl::onSelection(
    void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer
)
{
  auto *self = static_cast<Impl *>(data);
  self->destroyOffer(self->selection);
  if (!offer)
    return;

  self->selection = offer;
  if (self->offered(offer, kOwnedMarker)) {
    LOG_DEBUG("clipboard: wayland selection is our own, ignoring");
    return;
  }
  self->selectionDirty = true;
}

void WlDataControlClipboard::Impl::onPrimarySelection(
    void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer
)
{
  auto *self = static_cast<Impl *>(data);
  self->destroyOffer(self->primary);
  self->primary = offer;
}

void WlDataControlClipboard::Impl::onFinished(void *data, ext_data_control_device_v1 *)
{
  LOG_WARN("clipboard: wayland data-control device finished, clipboard sharing stops");
  static_cast<Impl *>(data)->finished = true;
}

void WlDataControlClipboard::Impl::onSend(void *data, ext_data_control_source_v1 *source, const char *mime, int32_t fd)
{
  auto *self = static_cast<Impl *>(data);
  if (source != self->source || !self->sourcePayload) {
    close(fd);
    return;
  }

  const std::string type = mime;
  const QByteArray *bytes = nullptr;
  if (type == kPngMime) {
    bytes = &self->sourcePayload->png;
  } else if (std::ranges::any_of(kOfferedTextMimes, [&](const char *m) { return type == m; })) {
    bytes = &self->sourcePayload->text;
  }

  // The marker, and any format we do not hold, is answered with an empty stream
  if (!bytes || bytes->isEmpty()) {
    LOG_DEBUG("clipboard: wayland reader asked for %s, nothing to send", mime);
    close(fd);
    return;
  }

  LOG_DEBUG("clipboard: wayland reader asked for %s, sending %lld bytes", mime, static_cast<long long>(bytes->size()));
  std::shared_ptr<const QByteArray> held(self->sourcePayload, bytes);
  auto gate = self->gate;
  gate->active++;
  try {
    std::thread([fd, held, gate] {
      writeToFd(fd, held, gate);
      gate->active--;
    }).detach();
  } catch (const std::exception &e) {
    LOG_ERR("clipboard: cannot start the sender thread: %s", e.what());
    gate->active--;
    close(fd);
  }
}

void WlDataControlClipboard::Impl::onCancelled(void *data, ext_data_control_source_v1 *source)
{
  auto *self = static_cast<Impl *>(data);
  LOG_DEBUG("clipboard: our wayland selection was replaced");
  if (source == self->source) {
    self->destroySource();
  } else {
    ext_data_control_source_v1_destroy(source);
  }
}

bool WlDataControlClipboard::Impl::offered(const ext_data_control_offer_v1 *offer, const char *mime) const
{
  const auto it = offers.find(const_cast<ext_data_control_offer_v1 *>(offer));
  if (it == offers.end())
    return false;
  return std::ranges::find(it->second, std::string_view(mime)) != it->second.end();
}

void WlDataControlClipboard::Impl::destroyOffer(ext_data_control_offer_v1 *&offer)
{
  if (!offer)
    return;
  offers.erase(offer);
  ext_data_control_offer_v1_destroy(offer);
  offer = nullptr;
}

void WlDataControlClipboard::Impl::destroySource()
{
  if (!source)
    return;
  ext_data_control_source_v1_destroy(source);
  source = nullptr;
  sourcePayload.reset();
}

void WlDataControlClipboard::Impl::wake() const
{
  const uint64_t one = 1;
  if (write(wakeFd, &one, sizeof(one)) < 0 && errno != EAGAIN)
    LOG_WARN("clipboard: cannot wake the wayland worker: %s", std::strerror(errno));
}

bool WlDataControlClipboard::Impl::connect()
{
  display = wl_display_connect(nullptr);
  if (!display) {
    LOG_WARN("clipboard: cannot connect to the wayland display, no clipboard sharing");
    return false;
  }

  registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, this);
  if (wl_display_roundtrip(display) < 0) {
    LOG_WARN("clipboard: wayland roundtrip failed, no clipboard sharing");
    teardown();
    return false;
  }

  if (!seat || !manager) {
    LOG_WARN(
        "clipboard: the compositor offers no %s, no clipboard sharing", seat ? "ext_data_control_manager_v1" : "wl_seat"
    );
    teardown();
    return false;
  }

  wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (wakeFd < 0) {
    LOG_WARN("clipboard: cannot create the wake-up descriptor: %s", std::strerror(errno));
    teardown();
    return false;
  }

  device = ext_data_control_manager_v1_get_data_device(manager, seat);
  ext_data_control_device_v1_add_listener(device, &kDeviceListener, this);
  return true;
}

void WlDataControlClipboard::Impl::teardown()
{
  available = false;
  if (display) {
    destroyOffer(selection);
    destroyOffer(primary);
    for (auto &[offer, mimes] : offers)
      ext_data_control_offer_v1_destroy(offer);
    offers.clear();
    destroySource();
    if (device)
      ext_data_control_device_v1_destroy(device);
    if (manager)
      ext_data_control_manager_v1_destroy(manager);
    if (seat)
      wl_seat_destroy(seat);
    if (registry)
      wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
  }
  device = nullptr;
  manager = nullptr;
  seat = nullptr;
  registry = nullptr;
  display = nullptr;
  if (wakeFd >= 0)
    close(wakeFd);
  wakeFd = -1;
}

bool WlDataControlClipboard::Impl::flush()
{
  while (wl_display_flush(display) < 0) {
    if (errno == EINTR)
      continue;
    if (errno != EAGAIN)
      return false;
    pollfd pfd{wl_display_get_fd(display), POLLOUT, 0};
    if (poll(&pfd, 1, kReadWaitMs) == 0)
      return false;
  }
  return true;
}

QByteArray WlDataControlClipboard::Impl::readOffer(ext_data_control_offer_v1 *offer, const char *mime, qint64 limit)
{
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) < 0) {
    LOG_WARN("clipboard: cannot create a pipe: %s", std::strerror(errno));
    return {};
  }

  ext_data_control_offer_v1_receive(offer, mime, fds[1]);
  close(fds[1]);
  if (!flush()) {
    LOG_WARN("clipboard: cannot flush the wayland request for %s", mime);
    close(fds[0]);
    return {};
  }

  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(kReadDeadlineMs);
  QByteArray contents;
  bool ok = false;
  while (true) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) {
      LOG_WARN("clipboard: timed out reading %s from the wayland selection", mime);
      break;
    }

    pollfd pfd{fds[0], POLLIN, 0};
    const int rc = poll(&pfd, 1, static_cast<int>(std::min<long long>(left, kReadWaitMs)));
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (rc == 0)
      continue;

    char chunk[kChunkBytes];
    const ssize_t n = read(fds[0], chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      break;
    }
    if (n == 0) {
      ok = true;
      break;
    }
    if (contents.size() + n > limit) {
      LOG_WARN("clipboard: %s exceeds the %lld byte limit, ignored", mime, static_cast<long long>(limit));
      break;
    }
    contents.append(chunk, n);
  }
  close(fds[0]);

  if (!ok)
    return {};
  LOG_DEBUG(
      "clipboard: read %lld bytes of %s from the wayland selection", static_cast<long long>(contents.size()), mime
  );
  return contents;
}

void WlDataControlClipboard::Impl::readSelection()
{
  auto *offer = selection;
  if (!offer)
    return;

  const qint64 limit = std::min(maxBytes ? maxBytes() : kMaxReadBytes, kMaxReadBytes);
  if (limit <= 0)
    return;

  QByteArray text;
  QByteArray dib;

  if (offered(offer, kPngMime)) {
    const auto png = readOffer(offer, kPngMime, limit);
    if (!png.isEmpty())
      dib = PortalClipboard::decodeFormat(IClipboard::Format::Bitmap, png);
  }

  for (const char *mime : kTextMimes) {
    if (!offered(offer, mime))
      continue;
    text = readOffer(offer, mime, limit);
    while (text.endsWith('\0'))
      text.chop(1);
    text.replace("\r\n", "\n");
    break;
  }

  if (text.isEmpty() && dib.isEmpty()) {
    LOG_DEBUG("clipboard: wayland selection has nothing usable, keeping the cache");
    return;
  }

  const auto digest = computeDigest(text, dib);
  {
    std::scoped_lock lock{mutex};
    if (digest == lastDigest) {
      LOG_DEBUG("clipboard: wayland selection is unchanged, skipping");
      return;
    }
    lastDigest = digest;
  }

  {
    std::scoped_lock lock{cacheMutex};
    cache->open(0);
    cache->empty();
    if (!text.isEmpty())
      cache->add(IClipboard::Format::Text, text.toStdString());
    if (!dib.isEmpty())
      cache->add(IClipboard::Format::Bitmap, dib.toStdString());
    cache->close();
  }

  LOG_DEBUG(
      "clipboard: local selection stored, text %lld bytes, bitmap %lld bytes", static_cast<long long>(text.size()),
      static_cast<long long>(dib.size())
  );
  if (onLocalChange)
    onLocalChange();
}

void WlDataControlClipboard::Impl::applyClaim(const std::shared_ptr<const Payload> &payload)
{
  destroySource();

  source = ext_data_control_manager_v1_create_data_source(manager);
  ext_data_control_source_v1_add_listener(source, &kSourceListener, this);
  sourcePayload = payload;

  if (!payload->text.isEmpty()) {
    for (const char *mime : kOfferedTextMimes)
      ext_data_control_source_v1_offer(source, mime);
  }
  if (!payload->png.isEmpty())
    ext_data_control_source_v1_offer(source, kPngMime);
  ext_data_control_source_v1_offer(source, kOwnedMarker);

  ext_data_control_device_v1_set_selection(device, source);
  LOG_DEBUG(
      "clipboard: offering the cache on wayland, text %lld bytes, png %lld bytes",
      static_cast<long long>(payload->text.size()), static_cast<long long>(payload->png.size())
  );
}

void WlDataControlClipboard::Impl::run()
{
  blockSigpipe();
  const int wlFd = wl_display_get_fd(display);

  while (!stop && !finished) {
    while (wl_display_prepare_read(display) != 0) {
      if (wl_display_dispatch_pending(display) < 0)
        finished = true;
    }
    if (finished) {
      wl_display_cancel_read(display);
      break;
    }

    bool needOut = false;
    if (wl_display_flush(display) < 0) {
      if (errno != EAGAIN) {
        wl_display_cancel_read(display);
        LOG_ERR("clipboard: wayland connection lost: %s", std::strerror(errno));
        break;
      }
      needOut = true;
    }

    pollfd fds[2] = {{wlFd, static_cast<short>(POLLIN | (needOut ? POLLOUT : 0)), 0}, {wakeFd, POLLIN, 0}};
    const int rc = poll(fds, 2, -1);
    if (rc < 0) {
      wl_display_cancel_read(display);
      if (errno == EINTR)
        continue;
      LOG_ERR("clipboard: poll failed: %s", std::strerror(errno));
      break;
    }

    if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
      if (wl_display_read_events(display) < 0 && errno != EAGAIN) {
        LOG_ERR("clipboard: wayland connection lost: %s", std::strerror(errno));
        break;
      }
    } else {
      wl_display_cancel_read(display);
    }
    if (wl_display_dispatch_pending(display) < 0) {
      LOG_ERR("clipboard: wayland dispatch failed: %s", std::strerror(errno));
      break;
    }

    if (fds[1].revents & POLLIN) {
      uint64_t counter;
      std::ignore = read(wakeFd, &counter, sizeof(counter));
    }
    if (stop || finished)
      break;

    std::shared_ptr<const Payload> claim;
    {
      std::scoped_lock lock{mutex};
      claim = std::move(pendingClaim);
    }
    if (claim)
      applyClaim(claim);

    if (selectionDirty) {
      selectionDirty = false;
      readSelection();
    }
  }

  teardown();
  LOG_DEBUG("clipboard: wayland data-control worker stopped");
}

WlDataControlClipboard::WlDataControlClipboard(
    EiClipboard *cache, MaxBytesCallback maxBytes, LocalChangeCallback onLocalChange
)
    : m_impl{std::make_unique<Impl>()}
{
  m_impl->cache = cache;
  m_impl->maxBytes = std::move(maxBytes);
  m_impl->onLocalChange = std::move(onLocalChange);

  if (!cache || !m_impl->connect())
    return;

  m_impl->available = true;
  try {
    m_impl->worker = std::thread([impl = m_impl.get()] { impl->run(); });
  } catch (const std::exception &e) {
    LOG_ERR("clipboard: cannot start the wayland worker: %s", e.what());
    m_impl->teardown();
    return;
  }
  LOG_INFO("clipboard: using wayland data-control (ext-data-control-v1)");
}

WlDataControlClipboard::~WlDataControlClipboard()
{
  if (m_impl->worker.joinable()) {
    m_impl->stop = true;
    m_impl->wake();
    m_impl->worker.join();
  }

  // Senders hold no pointer to us, but give them a moment so no fd outlives the object
  m_impl->gate->stop = true;
  for (int i = 0; i < 300 && m_impl->gate->active > 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

bool WlDataControlClipboard::isAvailable() const
{
  return m_impl->available;
}

void WlDataControlClipboard::claimFromCache()
{
  if (!m_impl->available)
    return;

  QByteArray text;
  QByteArray dib;
  {
    std::scoped_lock lock{m_impl->cacheMutex};
    m_impl->cache->open(0);
    if (m_impl->cache->has(IClipboard::Format::Text))
      text = QByteArray::fromStdString(m_impl->cache->get(IClipboard::Format::Text));
    if (m_impl->cache->has(IClipboard::Format::Bitmap))
      dib = QByteArray::fromStdString(m_impl->cache->get(IClipboard::Format::Bitmap));
    m_impl->cache->close();
  }

  auto payload = std::make_shared<Payload>();
  payload->text = std::move(text);
  if (!dib.isEmpty()) {
    payload->png = PortalClipboard::encodeFormat(IClipboard::Format::Bitmap, dib);
    if (payload->png.isEmpty()) {
      LOG_WARN("clipboard: cannot convert the bitmap for wayland, offering text only");
      dib.clear();
    }
  }
  if (payload->text.isEmpty() && payload->png.isEmpty()) {
    LOG_DEBUG("clipboard: cache is empty, nothing to offer on wayland");
    return;
  }

  {
    std::scoped_lock lock{m_impl->mutex};
    m_impl->lastDigest = computeDigest(payload->text, dib);
    m_impl->pendingClaim = std::move(payload);
  }
  m_impl->wake();
}

} // namespace deskflow
