#include "EditorCamera.h"
#include "Engine/Scene/Camera/CameraComponent.h"
#include "Engine/Renderer/CascadedShadowMap.h"
#include <DirectXMath.h>
#include <cmath>
#include <algorithm>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>

using namespace DirectX;
using Camera = Editor::EditorCamera;
using VM = Camera::ViewMode;

namespace {
void Require(bool value, const char* message)
{
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

bool Near(float a, float b, float tolerance = 0.001f) { return std::fabs(a - b) < tolerance; }
bool SamePosition(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z);
}

void TestViewSwitchPreservesPositionAndZoom()
{
    SE::CameraComponent component;
    Camera camera;
    camera.SyncFromCamera(component);
    const XMFLOAT3 eye = component.eye;
    camera.SetViewMode(VM::Top, component);
    camera.Zoom(4.0f, component);
    const float zoom = component.orthoHeight;
    const VM modes[] = { VM::Bottom, VM::Left, VM::Right, VM::Front, VM::Back,
                         VM::Isometric, VM::Perspective, VM::Top, VM::Orthographic };
    for (VM mode : modes)
    {
        camera.SetViewMode(mode, component);
        Require(SamePosition(eye, component.eye), "view switch moved the eye");
        Require(Near(zoom, component.orthoHeight), "view switch reset orthographic zoom");
        Require(component.orthographic == (mode != VM::Perspective), "projection mode mismatch");
        const float determinant = XMVectorGetX(XMMatrixDeterminant(component.GetViewMatrix()));
        Require(std::isfinite(determinant) && Near(determinant, 1.0f), "singular camera basis");
    }
}

void TestExactAxisViews()
{
    SE::CameraComponent component;
    Camera camera;
    camera.SyncFromCamera(component);
    struct Expected { VM mode; XMFLOAT3 direction; };
    const Expected cases[] = {
        { VM::Top, { 0, -1, 0 } }, { VM::Bottom, { 0, 1, 0 } },
        { VM::Left, { 1, 0, 0 } }, { VM::Right, { -1, 0, 0 } },
        { VM::Front, { 0, 0, 1 } }, { VM::Back, { 0, 0, -1 } }
    };
    for (const auto& test : cases)
    {
        camera.SetViewMode(test.mode, component);
        XMFLOAT3 direction;
        XMStoreFloat3(&direction, XMLoadFloat3(&component.target) - XMLoadFloat3(&component.eye));
        Require(SamePosition(direction, test.direction), "axis view isn't aligned");
        Require(Near(XMVectorGetX(XMVector3Dot(XMLoadFloat3(&component.up), XMLoadFloat3(&direction))), 0),
                "up is not perpendicular to the view direction");
    }
}

void TestPanTracksPixels()
{
    SE::CameraComponent component;
    Camera camera;
    camera.SyncFromCamera(component);
    camera.SetViewMode(VM::Top, component);
    component.orthoHeight = 20.0f;
    const XMFLOAT3 before = component.eye;
    camera.Pan(100, 50, 1000, component);
    Require(Near(component.eye.x, before.x - 2), "horizontal ortho pan scale");
    Require(Near(component.eye.y, before.y), "top pan moved along view axis");
    Require(Near(component.eye.z, before.z + 1), "vertical ortho pan scale");
    const XMFLOAT3 first = component.eye;
    camera.Pan(50, 25, 500, component);
    Require(Near(component.eye.x - first.x, -2), "pan changed with proportional resize");
    Require(Near(component.eye.z - first.z, 1), "vertical pan changed with resize");
}

void TestFocusFitsPortraitAndLandscape()
{
    SE::CameraComponent component;
    Camera camera;
    camera.SyncFromCamera(component);
    const XMFLOAT3 center = { 3, 5, 9 };
    for (float aspect : { 0.5f, 1.0f, 2.0f })
    {
        for (VM mode : { VM::Perspective, VM::Top, VM::Isometric })
        {
            camera.SetViewMode(mode, component);
            camera.FocusOn(center, 4, component, aspect);
            const XMMATRIX vp = component.GetViewMatrix() * component.GetProjectionMatrix(aspect);
            const XMVECTOR ndc = XMVector3TransformCoord(XMLoadFloat3(&center), vp);
            Require(Near(XMVectorGetX(ndc), 0) && Near(XMVectorGetY(ndc), 0), "focus not centered");
            if (component.orthographic)
                Require(component.orthoHeight * (std::min)(aspect, 1.0f) >= 8,
                        "ortho focus cropped selection");
            else
            {
                const float halfY = XMConvertToRadians(component.fovDeg) * 0.5f;
                const float halfX = atanf(tanf(halfY) * aspect);
                Require(camera.orbitDistance * sinf((std::min)(halfX, halfY)) >= 4,
                        "perspective focus cropped sphere");
            }
        }
    }
}

void TestOrthoPickRaysAreParallel()
{
    SE::CameraComponent component;
    Camera camera;
    camera.SyncFromCamera(component);
    camera.SetViewMode(VM::Top, component);
    const XMMATRIX inverse = XMMatrixInverse(nullptr, component.GetViewMatrix() * component.GetProjectionMatrix(1.5f));
    const XMVECTOR a0 = XMVector3TransformCoord(XMVectorSet(-0.5f, 0.5f, 0, 1), inverse);
    const XMVECTOR a1 = XMVector3TransformCoord(XMVectorSet(-0.5f, 0.5f, 1, 1), inverse);
    const XMVECTOR b0 = XMVector3TransformCoord(XMVectorSet(0.5f, -0.5f, 0, 1), inverse);
    const XMVECTOR b1 = XMVector3TransformCoord(XMVectorSet(0.5f, -0.5f, 1, 1), inverse);
    Require(Near(XMVectorGetX(XMVector3Dot(XMVector3Normalize(a1 - a0), XMVector3Normalize(b1 - b0))), 1),
            "orthographic pick rays diverge");
    Require(XMVectorGetX(XMVector3Length(a0 - b0)) > 1, "orthographic rays share a perspective origin");
}

void TestOrthographicShadowVolume()
{
    SE::CameraComponent component;
    Camera camera;
    camera.SyncFromCamera(component);
    camera.SetViewMode(VM::Top, component);
    component.orthoHeight = 20.0f;
    SE::CascadedShadowMap shadows;
    const XMMATRIX view = component.GetViewMatrix();
    shadows.Update({ 0.4f, 0.8f, 0.3f }, view, component.GetProjectionMatrix(1.5f),
                   component.nearZ, component.farZ);
    float nearZ = -component.farZ;
    for (int cascade = 0; cascade < SE::CSM_NUM_CASCADES; ++cascade)
    {
        const float farZ = shadows.GetSplitFar(cascade);
        Require(std::isfinite(farZ) && farZ > nearZ, "invalid orthographic shadow split");
        const XMMATRIX viewToShadow = XMMatrixInverse(nullptr, view) * shadows.GetCascadeVP(cascade);
        for (int corner = 0; corner < 8; ++corner)
        {
            const XMVECTOR point = XMVectorSet((corner & 1) ? 15.0f : -15.0f,
                (corner & 2) ? 10.0f : -10.0f, (corner & 4) ? farZ : nearZ, 1);
            XMFLOAT3 ndc;
            XMStoreFloat3(&ndc, XMVector3TransformCoord(point, viewToShadow));
            Require(std::isfinite(ndc.x) && std::isfinite(ndc.y) && std::isfinite(ndc.z), "non-finite shadow matrix");
            Require(std::fabs(ndc.x) <= 1.01f && std::fabs(ndc.y) <= 1.01f && ndc.z >= -0.01f && ndc.z <= 1.01f,
                    "orthographic corner outside its shadow cascade");
        }
        nearZ = farZ;
    }
    Require(Near(shadows.GetSplitFar(0), -component.farZ / 3.0f, 0.01f), "ortho splits aren't linear");
}

void TestZoomClampDoesNotOvershootPivot()
{
    SE::CameraComponent component;
    Camera camera;
    camera.SyncFromCamera(component);
    const float initialDistance = camera.orbitDistance;
    const XMVECTOR direction = XMVector3Normalize(XMLoadFloat3(&component.target) - XMLoadFloat3(&component.eye));
    const XMVECTOR pivot = XMLoadFloat3(&component.eye) + direction * initialDistance;
    camera.Zoom(100000, component);
    const XMVECTOR remaining = pivot - XMLoadFloat3(&component.eye);
    Require(Near(XMVectorGetX(XMVector3Length(remaining)), camera.orbitDistance), "zoom clamp crossed pivot");
    Require(XMVectorGetX(XMVector3Dot(remaining, direction)) > 0, "zoom went past focus");
}
}

int main()
{
    TestViewSwitchPreservesPositionAndZoom();
    TestExactAxisViews();
    TestPanTracksPixels();
    TestFocusFitsPortraitAndLandscape();
    TestOrthoPickRaysAreParallel();
    TestZoomClampDoesNotOvershootPivot();
    TestOrthographicShadowVolume();
    std::puts("EditorCameraTests: all seven groups passed");
    return 0;
}
