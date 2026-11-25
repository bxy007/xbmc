/*
 *  Copyright (C) 2005-2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDStateSerializer.h"

#include "utils/StringUtils.h"
#include "utils/XBMCTinyXML2.h"
#include "utils/log.h"

#include <charconv>
#include <cstring>

namespace
{
// Serializer version - used to avoid processing deprecated/legacy schemas
constexpr int DVDSTATESERIALIZER_VERSION = 2;

template<typename T>
bool ParseNumber(const char* text, T& value)
{
  if (!text)
    return false;
  const char* end = text + std::strlen(text);
  const auto [ptr, error] = std::from_chars(text, end, value);
  return error == std::errc{} && ptr == end;
}
}

bool CDVDStateSerializer::DVDStateToXML(std::string& xmlstate, const DVDState& state)
{
  CXBMCTinyXML2 xmlDoc;

  auto* eRoot = xmlDoc.NewElement("navstate");
  if (eRoot == nullptr)
    return false;

  eRoot->SetAttribute("version", DVDSTATESERIALIZER_VERSION);

  AddXMLElement(*eRoot, "title", std::to_string(state.title).c_str());
  AddXMLElement(*eRoot, "pgn", std::to_string(state.pgn).c_str());
  AddXMLElement(*eRoot, "pgcn", std::to_string(state.pgcn).c_str());
  AddXMLElement(*eRoot, "current_angle", std::to_string(state.current_angle).c_str());
  AddXMLElement(*eRoot, "audio_num", std::to_string(state.audio_num).c_str());
  AddXMLElement(*eRoot, "subp_num", std::to_string(state.subp_num).c_str());
  AddXMLElement(*eRoot, "sub_enabled", state.sub_enabled ? "true" : "false");
  xmlDoc.InsertEndChild(eRoot);

  tinyxml2::XMLPrinter printer;
  xmlDoc.Accept(&printer);
  xmlstate = printer.CStr();
  return true;
}

bool CDVDStateSerializer::XMLToDVDState(DVDState& state, const std::string& xmlstate)
{
  state = {};
  CXBMCTinyXML2 xmlDoc;

  if (!xmlDoc.Parse(xmlstate))
    return false;

  tinyxml2::XMLHandle hRoot(xmlDoc.RootElement());
  if (!hRoot.ToElement() || !StringUtils::EqualsNoCase(hRoot.ToElement()->Value(), "navstate"))
  {
    CLog::LogF(LOGERROR, "Failed to deserialize dvd state - failed to detect root element.");
    return false;
  }

  auto version = hRoot.ToElement()->Attribute("version");
  if (!version || !StringUtils::EqualsNoCase(version, std::to_string(DVDSTATESERIALIZER_VERSION)))
  {
    CLog::LogF(LOGERROR, "Failed to deserialize dvd state - incompatible serializer version.");
    return false;
  }

  const auto* childElement = hRoot.ToElement()->FirstChildElement();
  while (childElement != nullptr)
  {
    const std::string property = childElement->Value();
    const char* text = childElement->GetText();
    bool valid = true;
    if (property == "title")
      valid = ParseNumber(text, state.title);
    else if (property == "pgn")
      valid = ParseNumber(text, state.pgn);
    else if (property == "pgcn")
      valid = ParseNumber(text, state.pgcn);
    else if (property == "current_angle")
      valid = ParseNumber(text, state.current_angle);
    else if (property == "subp_num")
      valid = ParseNumber(text, state.subp_num);
    else if (property == "audio_num")
      valid = ParseNumber(text, state.audio_num);
    else if (property == "sub_enabled")
      state.sub_enabled = text && StringUtils::EqualsNoCase(text, "true");
    else
      CLog::LogF(LOGWARNING, "Unmapped dvd state property {}, ignored.", childElement->Value());
    if (!valid)
      return false;
    childElement = childElement->NextSiblingElement();
  }
  return state.title > 0 && state.pgn > 0 && state.pgcn > 0;
}

void CDVDStateSerializer::AddXMLElement(tinyxml2::XMLElement& root,
                                        const std::string& name,
                                        const std::string& value)
{
  auto* xmlDoc = root.GetDocument();
  auto* xmlElement = xmlDoc->NewElement(name.c_str());
  auto* xmlElementValue = xmlDoc->NewText(value.c_str());
  xmlElement->InsertEndChild(xmlElementValue);
  root.InsertEndChild(xmlElement);
}
