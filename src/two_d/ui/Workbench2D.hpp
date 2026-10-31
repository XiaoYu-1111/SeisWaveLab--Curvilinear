#pragma once
namespace Seis2D::Curvilinear::Ui {
int runWorkbench2D(bool smoke = false, bool returnHomeTest = false, bool imagingRouteTest = false, bool viscoelastic=false);
int validateModelWorkflow2D();
int validateAttenuationWorkflow2D();
int captureWorkflow2D();
int validateCfsPmlWorkflow2D();
int validatePrecisionWorkflow2D();
int validateThemePanels2D();
}
