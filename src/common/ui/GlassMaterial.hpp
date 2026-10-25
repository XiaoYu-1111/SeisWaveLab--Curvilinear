#pragma once
#include "imgui.h"
#include <string>
namespace SeisWave::Ui {
void installGlassPresentation();
void markGlassPanel();
void glassPrepareBackdrop();
void glassPreparePopups();
void destroyGlassBackdrop();
int glassBackdropPasses();
const std::string& glassBackdropError();
}
