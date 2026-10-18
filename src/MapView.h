#pragma once

#include "Models.h"

#include <windows.h>

#include <vector>

constexpr UINT WM_IGS_MAP_TOGGLE_STATION = WM_APP + 40;
constexpr UINT WM_IGS_MAP_AREA_SELECTED = WM_APP + 41;

struct MapSelectionBounds {
    double minimumLatitude = -90.0;
    double maximumLatitude = 90.0;
    double minimumLongitude = -180.0;
    double maximumLongitude = 180.0;
};

bool RegisterMapViewClass(HINSTANCE instance);
HWND CreateMapView(HWND parent, HINSTANCE instance, int controlId);
void MapViewSetStations(HWND mapWindow, const std::vector<StationFile>* stations);
void MapViewRefresh(HWND mapWindow);
void MapViewResetView(HWND mapWindow);
void MapViewSetRectangleSelectionMode(HWND mapWindow, bool enabled);
