#include "AttractSlides.h"

#include <yaml-cpp/yaml.h>

#include <filesystem>

namespace AttractSlides
{

namespace
{

Marker::Pointer PointerFrom(const std::string& value)
{
  if (value == "right") return Marker::Pointer::Right;
  if (value == "above") return Marker::Pointer::Above;
  if (value == "below") return Marker::Pointer::Below;
  return Marker::Pointer::Left;
}

}  // namespace

bool Load(const std::string& gameFolder, std::vector<Slide>* slides, std::string* error)
{
  if (slides == nullptr)
  {
    return false;
  }
  slides->clear();

  const std::filesystem::path folder = std::filesystem::path(gameFolder) / "slides";
  const std::filesystem::path manifest = folder / "slides.yaml";
  std::error_code ec;
  if (!std::filesystem::exists(manifest, ec))
  {
    // A game with no slides, which is most of them.
    return true;
  }

  try
  {
    const YAML::Node root = YAML::LoadFile(manifest.string());
    const YAML::Node list = root["slides"];
    if (!list || !list.IsSequence())
    {
      if (error) *error = manifest.string() + ": no 'slides' sequence";
      return false;
    }

    for (const YAML::Node& entry : list)
    {
      Slide slide;
      if (entry["title"]) slide.title = entry["title"].as<std::string>("");
      if (entry["text"]) slide.text = entry["text"].as<std::string>("");
      if (entry["durationMs"]) slide.durationMs = entry["durationMs"].as<uint32_t>(0);
      if (entry["image"])
      {
        const std::string image = entry["image"].as<std::string>("");
        if (!image.empty())
        {
          // Resolved against the folder here, so nothing downstream has to
          // know where the game folder was.
          slide.imagePath = (folder / image).string();
        }
      }

      const YAML::Node markers = entry["markers"];
      if (markers && markers.IsSequence())
      {
        for (const YAML::Node& node : markers)
        {
          if (!node["x"] || !node["y"])
          {
            continue;
          }
          Marker marker;
          marker.x = node["x"].as<float>(0.0f);
          marker.y = node["y"].as<float>(0.0f);
          if (marker.x < 0.0f || marker.x > 1.0f || marker.y < 0.0f || marker.y > 1.0f)
          {
            continue;
          }
          if (node["number"]) marker.number = node["number"].as<int>(0);
          if (node["pointer"]) marker.pointer = PointerFrom(node["pointer"].as<std::string>(""));
          slide.markers.push_back(marker);
        }
      }

      // A slide with neither a picture nor words is a blank screen.
      if (!slide.imagePath.empty() || !slide.text.empty() || !slide.title.empty())
      {
        slides->push_back(std::move(slide));
      }
    }
  }
  catch (const std::exception& e)
  {
    if (error) *error = manifest.string() + ": " + e.what();
    slides->clear();
    return false;
  }

  return true;
}

}  // namespace AttractSlides
