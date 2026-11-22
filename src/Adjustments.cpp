#include "Adjustments.h"

#include <algorithm>
#include <cmath>

bool Adjustments::Validate(const Definition& d, std::string* error)
{
  auto fail = [&](const std::string& why)
  {
    if (error) *error = "adjustment '" + d.name + "': " + why;
    return false;
  };
  if (!MachineState::IsValidName(d.name)) return fail("a name takes letters, digits, '_', '.' and '-'");
  if (d.label.empty()) return fail("needs a label");
  switch (d.kind)
  {
    case Kind::Number:
      if (!(d.min <= d.max)) return fail("min is above max");
      if (!(d.step > 0.0)) return fail("step must be above 0");
      if (d.defaultValue.type != StateValue::Type::Number) return fail("the default must be a number");
      if (d.defaultValue.number < d.min || d.defaultValue.number > d.max) return fail("the default is out of range");
      break;
    case Kind::Boolean:
      if (d.defaultValue.type != StateValue::Type::Boolean) return fail("the default must be true or false");
      break;
    case Kind::Choice:
      if (d.choices.empty()) return fail("needs at least one choice");
      if (d.defaultValue.type != StateValue::Type::String ||
          std::find(d.choices.begin(), d.choices.end(), d.defaultValue.text) == d.choices.end())
      {
        return fail("the default must be one of the choices");
      }
      break;
  }
  return true;
}

bool Adjustments::Fit(const Definition& d, const StateValue& in, StateValue* out)
{
  switch (d.kind)
  {
    case Kind::Number:
    {
      if (in.type != StateValue::Type::Number || !std::isfinite(in.number)) return false;
      // Onto the grid min + k * step, then into the range.
      double v = d.min + std::round((in.number - d.min) / d.step) * d.step;
      v = std::clamp(v, d.min, d.max);
      *out = StateValue::Number(v);
      return true;
    }
    case Kind::Boolean:
      if (in.type == StateValue::Type::Boolean)
      {
        *out = in;
        return true;
      }
      // 0 and 1 from a hand-edited file or a script.
      if (in.type == StateValue::Type::Number && (in.number == 0.0 || in.number == 1.0))
      {
        *out = StateValue::Boolean(in.number != 0.0);
        return true;
      }
      return false;
    case Kind::Choice:
      if (in.type != StateValue::Type::String) return false;
      if (std::find(d.choices.begin(), d.choices.end(), in.text) == d.choices.end()) return false;
      *out = in;
      return true;
  }
  return false;
}

bool Adjustments::Define(Definition definition, std::string* error)
{
  if (!Validate(definition, error)) return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_entries.find(definition.name);
  StateValue value = definition.defaultValue;
  if (it != m_entries.end())
  {
    StateValue kept;
    if (Fit(definition, it->second.value, &kept)) value = kept;
    it->second.definition = std::move(definition);
    it->second.value = value;
    return true;
  }

  auto pending = m_pending.find(definition.name);
  if (pending != m_pending.end())
  {
    StateValue stored;
    if (Fit(definition, pending->second, &stored))
    {
      value = stored;
    }
    // A stored value that no longer fits is dropped, and the file no longer
    // carries it once it is written again.
    m_pending.erase(pending);
  }
  m_order.push_back(definition.name);
  const std::string name = definition.name;
  m_entries.emplace(name, Entry{std::move(definition), value});
  return true;
}

bool Adjustments::Has(const std::string& name) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_entries.count(name) != 0;
}

std::string Adjustments::GroupOf(const std::string& name) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_entries.find(name);
  return it == m_entries.end() ? std::string() : it->second.definition.group;
}

StateValue Adjustments::Get(const std::string& name) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_entries.find(name);
  return it == m_entries.end() ? StateValue{} : it->second.value;
}

double Adjustments::GetNumber(const std::string& name, double fallback) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_entries.find(name);
  if (it == m_entries.end() || it->second.value.type != StateValue::Type::Number) return fallback;
  return it->second.value.number;
}

bool Adjustments::GetBoolean(const std::string& name, bool fallback) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_entries.find(name);
  if (it == m_entries.end() || it->second.value.type != StateValue::Type::Boolean) return fallback;
  return it->second.value.boolean;
}

bool Adjustments::Set(const std::string& name, const StateValue& value, StateValue* applied, std::string* error)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_entries.find(name);
  if (it == m_entries.end())
  {
    if (error) *error = "no adjustment '" + name + "'";
    return false;
  }
  StateValue fitted;
  if (!Fit(it->second.definition, value, &fitted))
  {
    if (error) *error = "adjustment '" + name + "' cannot take that value";
    return false;
  }
  if (fitted != it->second.value)
  {
    it->second.value = fitted;
    m_dirty = true;
  }
  if (applied) *applied = fitted;
  return true;
}

StateValue Adjustments::Stepped(const std::string& name, int direction) const
{
  return Stepped(name, Get(name), direction);
}

StateValue Adjustments::Stepped(const std::string& name, const StateValue& from, int direction) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_entries.find(name);
  if (it == m_entries.end()) return {};
  const Definition& d = it->second.definition;
  StateValue v;
  if (!Fit(d, from, &v)) v = it->second.value;
  switch (d.kind)
  {
    case Kind::Number:
    {
      const double next = v.number + (direction > 0 ? d.step : -d.step);
      return StateValue::Number(std::clamp(next, d.min, d.max));
    }
    case Kind::Boolean:
      return StateValue::Boolean(!v.boolean);
    case Kind::Choice:
    {
      const auto pos = std::find(d.choices.begin(), d.choices.end(), v.text);
      const int count = static_cast<int>(d.choices.size());
      int index = pos == d.choices.end() ? 0 : static_cast<int>(pos - d.choices.begin());
      index = (index + (direction > 0 ? 1 : -1) + count) % count;
      return StateValue::String(d.choices[index]);
    }
  }
  return v;
}

bool Adjustments::ResetToDefault(const std::string& name)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_entries.find(name);
  if (it == m_entries.end()) return false;
  if (it->second.value != it->second.definition.defaultValue)
  {
    it->second.value = it->second.definition.defaultValue;
    m_dirty = true;
  }
  return true;
}

std::string Adjustments::Display(const Definition& d, const StateValue& value)
{
  switch (d.kind)
  {
    case Kind::Number:
    {
      std::string text = MachineState::FormatNumber(value.number);
      if (!d.unit.empty()) text += " " + d.unit;
      return text;
    }
    case Kind::Boolean:
      return value.boolean ? "Yes" : "No";
    case Kind::Choice:
      return value.text;
  }
  return {};
}

std::string Adjustments::Display(const std::string& name) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_entries.find(name);
  if (it == m_entries.end()) return {};
  return Display(it->second.definition, it->second.value);
}

std::vector<Adjustments::Entry> Adjustments::List() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<Entry> list;
  list.reserve(m_order.size());
  for (const std::string& name : m_order)
  {
    list.push_back(m_entries.at(name));
  }
  return list;
}

void Adjustments::Load(const StateEntries& entries)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  for (const auto& [name, value] : entries)
  {
    auto it = m_entries.find(name);
    if (it == m_entries.end())
    {
      m_pending[name] = value;
      continue;
    }
    StateValue fitted;
    if (Fit(it->second.definition, value, &fitted))
    {
      it->second.value = fitted;
    }
  }
}

StateEntries Adjustments::Save() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  StateEntries entries;
  for (const std::string& name : m_order)
  {
    const Entry& entry = m_entries.at(name);
    if (entry.value != entry.definition.defaultValue)
    {
      entries.emplace_back(name, entry.value);
    }
  }
  std::vector<std::string> pendingNames;
  for (const auto& [name, value] : m_pending) pendingNames.push_back(name);
  std::sort(pendingNames.begin(), pendingNames.end());
  for (const std::string& name : pendingNames) entries.emplace_back(name, m_pending.at(name));
  return entries;
}

bool Adjustments::TakeDirty()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const bool dirty = m_dirty;
  m_dirty = false;
  return dirty;
}
