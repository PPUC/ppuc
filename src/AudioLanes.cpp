#include "AudioLanes.h"

#include <algorithm>
#include <unordered_map>

namespace AudioLanes
{
namespace
{

// Distance from the head of the override chain. A cycle -- which a
// misconfigured pair of plugins could publish -- is broken by the visit limit
// rather than by recursing forever.
unsigned int ChainDepth(const std::unordered_map<uint64_t, uint64_t>& overriderOf, uint64_t id, size_t laneCount)
{
  unsigned int depth = 0;
  uint64_t current = id;
  for (size_t guard = 0; guard <= laneCount; ++guard)
  {
    auto it = overriderOf.find(current);
    if (it == overriderOf.end())
    {
      return depth;
    }
    current = it->second;
    ++depth;
  }
  return depth;
}

}  // namespace

void Table::SetSources(const std::vector<Source>& sources, uint64_t nowMs)
{
  // What each source is overridden *by*, so a lane can find the lane that
  // silences it. The published direction is the other way round.
  std::unordered_map<uint64_t, uint64_t> overriderOf;
  for (const Source& source : sources)
  {
    if (source.overrideId != 0 && source.id != 0 && source.overrideId != source.id)
    {
      overriderOf[source.overrideId] = source.id;
    }
  }

  std::vector<Lane> next;
  next.reserve(sources.size());
  for (const Source& source : sources)
  {
    Lane lane;
    lane.id = source.id;
    lane.overrideId = source.overrideId;
    lane.name = source.name;
    lane.overridden = overriderOf.find(source.id) != overriderOf.end();
    lane.depth = ChainDepth(overriderOf, source.id, sources.size());

    const Lane* previous = Find(source.id);
    lane.lastAudibleMs = previous != nullptr ? previous->lastAudibleMs : nowMs;
    next.push_back(std::move(lane));
  }

  // Decided in this order, so ShouldPlay for an overridden lane always reads an
  // overrider whose audibility is already up to date for this block.
  std::stable_sort(next.begin(), next.end(), [](const Lane& a, const Lane& b) { return a.depth < b.depth; });

  lanes_ = std::move(next);
}

const Lane* Table::FindOverriderOf(uint64_t sourceId) const
{
  auto it =
      std::find_if(lanes_.begin(), lanes_.end(), [sourceId](const Lane& lane) { return lane.overrideId == sourceId; });
  return it == lanes_.end() ? nullptr : &*it;
}

const Lane* Table::Find(uint64_t sourceId) const
{
  auto it = std::find_if(lanes_.begin(), lanes_.end(), [sourceId](const Lane& lane) { return lane.id == sourceId; });
  return it == lanes_.end() ? nullptr : &*it;
}

bool Table::ShouldPlay(uint64_t sourceId, uint64_t nowMs) const
{
  const Lane* lane = Find(sourceId);
  if (lane == nullptr || !lane->overridden)
  {
    return true;
  }
  if (mode_ == OverrideMode::Replace)
  {
    return false;
  }

  // Fallback: audible only once everything layered above has gone quiet. The
  // whole chain is walked, not just the immediate overrider, so a three-deep
  // chain cannot let the bottom through under a still-playing top.
  uint64_t current = sourceId;
  for (size_t guard = 0; guard < lanes_.size(); ++guard)
  {
    const Lane* overrider = FindOverriderOf(current);
    if (overrider == nullptr)
    {
      return true;
    }
    const uint64_t silentForMs = nowMs > overrider->lastAudibleMs ? nowMs - overrider->lastAudibleMs : 0;
    if (silentForMs < fallbackHoldMs_)
    {
      return false;
    }
    current = overrider->id;
  }
  return true;
}

void Table::NoteMixed(uint64_t sourceId, bool audible, uint64_t nowMs)
{
  for (Lane& lane : lanes_)
  {
    if (lane.id != sourceId)
    {
      continue;
    }
    if (audible)
    {
      lane.lastAudibleMs = nowMs;
    }
    return;
  }
}

}  // namespace AudioLanes
