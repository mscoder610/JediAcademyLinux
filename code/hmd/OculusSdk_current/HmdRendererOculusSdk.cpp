#include "HmdRendererOculusSdk.h"
#include "../../renderer/tr_local.h"
#include "HmdDeviceOculusSdk.h"
#include "../HmdRenderer/PlatformInfo.h"

#include <OVR_CAPI.h>
#include <Extras/OVR_Math.h>
#include <OVR_CAPI_GL.h>

#include <math.h>
#include <iostream>
#include <algorithm>

#ifdef _WINDOWS
#include <stdlib.h>
#include <dwmapi.h>
#endif

using namespace OVR;
using namespace std;

HmdRendererOculusSdk::HmdRendererOculusSdk(HmdDeviceOculusSdk* pHmdDeviceOculusSdk)
    :mIsInitialized(false)
    ,mStartedFrame(false)
    ,mFrameStartTime(0)
    ,mStartedRendering(false)
    ,mEyeId(-1)
    ,mWindowWidth(0)
    ,mWindowHeight(0)
    ,mRenderWidth(0)
    ,mRenderHeight(0)
    ,mGuiScale(0.5f)
    ,mGuiOffsetFactorX(0)
    ,mMeterToGameUnits(1.0f)
    ,mAllowZooming(false)
    ,mUseMirrorTexture(true)
    ,mpDevice(pHmdDeviceOculusSdk)
    ,mpHmd(NULL)
    ,mpMirrorTexture(nullptr)
    ,mReadFBO(0)
    ,mCurrentUiMode(INGAME_HUD)
{
    mMeterToGameUnits =  39.3701f * 0.5f; // meter to inch * JA level factor 2?
}

HmdRendererOculusSdk::~HmdRendererOculusSdk()
{

}

bool HmdRendererOculusSdk::Init(int windowWidth, int windowHeight, PlatformInfo platformInfo)
{
    if (mpDevice == NULL || mpDevice->GetHmd() == NULL)
    {
        return false;
    }

    PreparePlatform();

    mWindowWidth = windowWidth;
    mWindowHeight = windowHeight;

    mpHmd = mpDevice->GetHmd();
    ovrHmdDesc desc = mpDevice->GetHmdDesc();

    
    mRenderWidth = desc.Resolution.w/2;
    mRenderHeight = desc.Resolution.h;
    
    if (desc.Type == ovrHmd_DK1)
    {
        mGuiScale = 0.3f;
        mGuiOffsetFactorX = 5.0f;
    }
    else if (desc.Type == ovrHmd_DK2)
    {
        mGuiScale = 0.50f;
        mGuiOffsetFactorX = 0;
        mAllowZooming = true;
    }
    

    // Configure Stereo settings.

    ovrSizei recommenedTex0Size = d_ovr_GetFovTextureSize(mpHmd, ovrEye_Left, desc.DefaultEyeFov[0], 1.0f);
    ovrSizei recommenedTex1Size = d_ovr_GetFovTextureSize(mpHmd, ovrEye_Right, desc.DefaultEyeFov[1], 1.0f);
    
    mRenderWidth = max(recommenedTex0Size.w, recommenedTex1Size.w);
    mRenderHeight = max(recommenedTex0Size.h, recommenedTex1Size.h);
   
    
    printf("HmdRendererOculusSdk: target texture size (%dx%d)\n", mRenderWidth, mRenderHeight);
    flush(std::cout);

    
    for (int i=0; i<FBO_COUNT; i++)
    {
        bool success = RenderTool::CreateFrameBufferWithoutTextures(mFboInfos[i], mRenderWidth, mRenderHeight);
        if (!success)
        {
            return false;
        }

        ovrResult worked = d_ovr_CreateSwapTextureSetGL(mpHmd, GL_SRGB8_ALPHA8, mRenderWidth, mRenderHeight, &(mEyeTextureSet[i]));
        if (worked != ovrSuccess)
        {
            return false;
        }

        worked = d_ovr_CreateSwapTextureSetGL(mpHmd, GL_DEPTH_COMPONENT24, mRenderWidth, mRenderHeight, &(mEyeTextureDepthSet[i]));
        if (worked != ovrSuccess)
        {
            return false;
        }
    }
    
    bool success = RenderTool::CreateFrameBufferWithoutTextures(mFboMenuInfo, mRenderWidth, mRenderHeight);
    if (!success)
    {
        return false;
    }
    

    ovrResult worked = d_ovr_CreateSwapTextureSetGL(mpHmd, GL_SRGB8_ALPHA8, mRenderWidth, mRenderHeight, &(mMenuTextureSet));
    if (worked != ovrSuccess)
    {
        return false;
    }

    worked = d_ovr_CreateSwapTextureSetGL(mpHmd, GL_DEPTH_COMPONENT24, mRenderWidth, mRenderHeight, &(mMenuTextureDepthSet));
    if (worked != ovrSuccess)
    {
        return false;
    }


    worked = d_ovr_CreateMirrorTextureGL(mpHmd, GL_SRGB8_ALPHA8, mWindowWidth, mWindowHeight, &mpMirrorTexture);
    if (worked != ovrSuccess)
    {
        return false;
    }

    ovrGLTexture* pTex = (ovrGLTexture*)mpMirrorTexture;

    // setup read buffer
    qglGenFramebuffers(1, &mReadFBO);
    qglBindFramebuffer(GL_READ_FRAMEBUFFER, mReadFBO);
    qglFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pTex->OGL.TexId, 0);
    qglFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    qglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);


    // Initialize VR structures, filling out description.
    ovrEyeRenderDesc eyeRenderDesc[2];
    eyeRenderDesc[0] = d_ovr_GetRenderDesc(mpHmd, ovrEye_Left, desc.DefaultEyeFov[0]);
    eyeRenderDesc[1] = d_ovr_GetRenderDesc(mpHmd, ovrEye_Right, desc.DefaultEyeFov[1]);
    mHmdToEyeViewOffset[0] = eyeRenderDesc[0].HmdToEyeViewOffset;
    //mHmdToEyeViewOffset[0].x *= mMeterToGameUnits;
    //mHmdToEyeViewOffset[0].y *= mMeterToGameUnits;
    //mHmdToEyeViewOffset[0].z *= mMeterToGameUnits;
    mHmdToEyeViewOffset[1] = eyeRenderDesc[1].HmdToEyeViewOffset;
    //mHmdToEyeViewOffset[1].x *= mMeterToGameUnits;
    //mHmdToEyeViewOffset[1].y *= mMeterToGameUnits;
    //mHmdToEyeViewOffset[1].z *= mMeterToGameUnits;
   
    // Initialize our single full screen Fov layer.
    mLayerMain.Header.Type      = ovrLayerType_EyeFov;
    mLayerMain.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft;
    mLayerMain.ColorTexture[0]  = mEyeTextureSet[0];
    mLayerMain.ColorTexture[1]  = mEyeTextureSet[1];
    mLayerMain.DepthTexture[0] = mEyeTextureDepthSet[0];
    mLayerMain.DepthTexture[1] = mEyeTextureDepthSet[1];
    mLayerMain.Fov[0]           = eyeRenderDesc[0].Fov;
    mLayerMain.Fov[1]           = eyeRenderDesc[1].Fov;
    mLayerMain.Viewport[0] = Recti(0, 0, mRenderWidth, mRenderHeight);
    mLayerMain.Viewport[1] = Recti(0, 0, mRenderWidth, mRenderHeight);


    // layer for fullscreen menus
    mLayerMenu.Header.Type = ovrLayerType_Quad;
    mLayerMenu.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft | ovrLayerFlag_HighQuality;
    mLayerMenu.ColorTexture = mMenuTextureSet;
    mLayerMenu.Viewport = Recti(0, 0, mRenderWidth, mRenderHeight);

    // fixed relative to their torso.
    mLayerMenu.QuadPoseCenter.Position.x = 0.00f;
    mLayerMenu.QuadPoseCenter.Position.y = 0.00f;
    mLayerMenu.QuadPoseCenter.Position.z = -3.00f;
    mLayerMenu.QuadPoseCenter.Orientation.x = 0;
    mLayerMenu.QuadPoseCenter.Orientation.y = 0;
    mLayerMenu.QuadPoseCenter.Orientation.z = 0;
    mLayerMenu.QuadPoseCenter.Orientation.w = 1;

    float quadSize = 3.0f;
    float uiAspect = 0.75f;

    mLayerMenu.QuadSize.x = quadSize;
    mLayerMenu.QuadSize.y = quadSize * uiAspect;


    // disable queue ahead - might cause black artefacts
    //d_ovr_SetBool(mpHmd, "QueueAheadEnabled", ovrFalse);


    mStartedRendering = false;
    mIsInitialized = true;

    return true;
}


void HmdRendererOculusSdk::Shutdown()
{
    if (!mIsInitialized)
    {
        return;
    }

    qglDeleteFramebuffers(1, &mReadFBO);
    mReadFBO = 0;

    d_ovr_DestroyMirrorTexture(mpHmd, mpMirrorTexture);
    mpMirrorTexture = nullptr;

    mpHmd = nullptr;

    mIsInitialized = false;
}

std::string HmdRendererOculusSdk::GetInfo()
{
    return "HmdRendererOculusSdk";
}

bool HmdRendererOculusSdk::HandlesSwap()
{
    return false;
}

bool HmdRendererOculusSdk::GetRenderResolution(int& rWidth, int& rHeight)
{
    if (!mIsInitialized)
    {
        return false;
    }

    rWidth = mRenderWidth;
    rHeight = mRenderHeight;

    return true;
}

void HmdRendererOculusSdk::StartFrame()
{
    mStartedFrame = true;
}


void HmdRendererOculusSdk::BeginRenderingForEye(bool leftEye)
{
    if (!mIsInitialized || !mStartedFrame)
    {
        return;
    }

    int fboId = 0;
    if (!leftEye && FBO_COUNT > 1)
    {
        fboId = 1;
    }

    mEyeId = fboId;

    if (!mStartedRendering)
    {
        // render begin
        qglDisable(GL_SCISSOR_TEST);
        qglBindVertexArray(0);
        qglUseProgramObjectARB(0);
        qglFrontFace(GL_CCW);
        
        qglEnable(GL_FRAMEBUFFER_SRGB);

        mStartedRendering = true;
        
        double displayMidpointSeconds = d_ovr_GetPredictedDisplayTime(mpHmd, 0);
        ovrTrackingState hmdState = d_ovr_GetTrackingState(mpHmd, displayMidpointSeconds, ovrTrue);
        d_ovr_CalcEyePoses(hmdState.HeadPose.ThePose, mHmdToEyeViewOffset, mLayerMain.RenderPose);

        for (int i=0; i<FBO_COUNT; i++)
        {
            ovrGLTexture* pTex = (ovrGLTexture*)&mEyeTextureSet[i]->Textures[mEyeTextureSet[i]->CurrentIndex];
            ovrGLTexture* pDepthTex = (ovrGLTexture*)&mEyeTextureDepthSet[i]->Textures[mEyeTextureDepthSet[i]->CurrentIndex];
            
            qglBindFramebuffer(GL_FRAMEBUFFER, mFboInfos[i].Fbo);
            qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,  pTex->OGL.TexId, 0);
            qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, pDepthTex->OGL.TexId, 0);

            RenderTool::ClearFBO(mFboInfos[i]);
        }


        ovrGLTexture* pMenuTex = (ovrGLTexture*)&mMenuTextureSet->Textures[mMenuTextureSet->CurrentIndex];
        ovrGLTexture* pMenuDepthTex = (ovrGLTexture*)&mMenuTextureDepthSet->Textures[mMenuTextureDepthSet->CurrentIndex];

        qglBindFramebuffer(GL_FRAMEBUFFER, mFboMenuInfo.Fbo);
        qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pMenuTex->OGL.TexId, 0);
        qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, pMenuDepthTex->OGL.TexId, 0);

        RenderTool::ClearFBO(mFboMenuInfo);
    }


    // bind framebuffer
    // this part can be called multiple times before the end of the frame

    if (mCurrentUiMode == INGAME_HUD)
    {
        qglBindFramebuffer(GL_FRAMEBUFFER, mFboInfos[mEyeId].Fbo);
    }
    else
    {
        if (leftEye)
        {
            qglBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        else
        {
            qglBindFramebuffer(GL_FRAMEBUFFER, mFboMenuInfo.Fbo);
        }
    }
}

void HmdRendererOculusSdk::EndFrame()
{
    if (!mIsInitialized || !mStartedFrame)
    {
        return;
    }
    
    if (mStartedFrame)
    {
        qglDisable(GL_SCISSOR_TEST);
        qglBindFramebuffer(GL_FRAMEBUFFER, 0);

        ovrLayerHeader* layers[2];
        layers[0] = &mLayerMain.Header;
        layers[1] = &mLayerMenu.Header;

        ovrViewScaleDesc viewScaleDesc;
        viewScaleDesc.HmdSpaceToWorldScaleInMeters = 1.0f / mMeterToGameUnits;
        viewScaleDesc.HmdToEyeViewOffset[0] = mHmdToEyeViewOffset[0];
        viewScaleDesc.HmdToEyeViewOffset[1] = mHmdToEyeViewOffset[1];

        ovrResult result = d_ovr_SubmitFrame(mpHmd, 0, &viewScaleDesc, layers, 2);


        for (int i=0; i<FBO_COUNT; i++)
        {
            mEyeTextureSet[i]->CurrentIndex = (mEyeTextureSet[i]->CurrentIndex + 1) % mEyeTextureSet[i]->TextureCount;
            mEyeTextureDepthSet[i]->CurrentIndex = (mEyeTextureDepthSet[i]->CurrentIndex + 1) % mEyeTextureDepthSet[i]->TextureCount;
        }

        mMenuTextureSet->CurrentIndex = (mMenuTextureSet->CurrentIndex + 1) % mMenuTextureSet->TextureCount;
        mMenuTextureDepthSet->CurrentIndex = (mMenuTextureDepthSet->CurrentIndex + 1) % mMenuTextureDepthSet->TextureCount;


        qglBindBuffer(GL_ARRAY_BUFFER, 0);
        qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        qglViewport(0, 0, mWindowWidth, mWindowHeight);

        if (mUseMirrorTexture)
        {
            // Do the blt
            qglBindFramebuffer(GL_READ_FRAMEBUFFER, mReadFBO);
            qglBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

            qglDrawBuffer(GL_BACK);

            qglBlitFramebuffer(0, mWindowHeight, mWindowWidth, 0,
                0, 0, mWindowWidth, mWindowHeight,
                GL_COLOR_BUFFER_BIT, GL_NEAREST);

            qglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        }

        mStartedFrame = false;
        mStartedRendering = false;


        // keep for debugging
        //RenderTool::DrawFbos(&mFboInfos[0], FBO_COUNT, mWindowWidth, mWindowHeight);
    }
    
    mEyeId = -1;
}


bool HmdRendererOculusSdk::GetCustomProjectionMatrix(float* rProjectionMatrix, float zNear, float zFar, float fov)
{
    if (!mIsInitialized)
    {
        return false;
    }

    ovrFovPort fovPort = mLayerMain.Fov[mEyeId];
    
    // ugly hardcoded default value
    if (mAllowZooming && fov < 124 || (mCurrentUiMode == FULLSCREEN_MENU))
    {
        // this calculation only works on DK2 at the moment
        
        // something needs zooming
        float fovRad = DEG2RAD(fov);
        float tanVal = tanf(fovRad * 0.5f);
        fovPort.DownTan = tanVal;
        fovPort.LeftTan = tanVal;
        fovPort.RightTan = tanVal;
        fovPort.UpTan = tanVal;
    }
    

    ovrMatrix4f projMatrix = d_ovrMatrix4f_Projection(fovPort, zNear, zFar, ovrProjection_RightHanded);
    ConvertMatrix(projMatrix, rProjectionMatrix);

    mLayerMain.ProjectionDesc = d_ovrTimewarpProjectionDesc_FromProjection(projMatrix, ovrProjection_RightHanded);

    return true;
}

bool HmdRendererOculusSdk::GetCustomViewMatrix(float* rViewMatrix, float& xPos, float& yPos, float& zPos, float bodyYaw, bool noPosition)
{
    if (!mIsInitialized)
    {
        return false;
    }

    ovrQuatf orientation = mLayerMain.RenderPose[mEyeId].Orientation;
    glm::quat currentOrientation = glm::quat(orientation.w, orientation.x, orientation.y, orientation.z);
    
    ovrVector3f position = mLayerMain.RenderPose[mEyeId].Position;
    glm::vec3 currentPosition = glm::vec3(position.x, position.y, position.z);
    
    if (mCurrentUiMode == FULLSCREEN_MENU)
    {
        currentOrientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        currentPosition = glm::vec3(0.0f, 0.0f, 0.0f);
    }

    // get current hmd rotation 
    glm::quat hmdRotation = glm::inverse(currentOrientation);

    // change hmd orientation to game coordinate system
    glm::quat convertHmdToGame = glm::rotate(glm::quat(1.0f,0.0f,0.0f,0.0f), (float)DEG2RAD(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    convertHmdToGame = glm::rotate(convertHmdToGame, (float)DEG2RAD(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    glm::mat4 hmdRotationMat = glm::mat4_cast(hmdRotation) * glm::mat4_cast(convertHmdToGame);


    // convert body transform to matrix
    glm::mat4 bodyPositionMat = glm::translate(glm::mat4(1.0f), glm::vec3(-xPos, -yPos, -zPos));
    glm::quat bodyYawRotation = glm::rotate(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), (float)(DEG2RAD(-bodyYaw)), glm::vec3(0.0f, 0.0f, 1.0f));

    

    glm::vec3 hmdPosition;
    hmdPosition.x = currentPosition.z * mMeterToGameUnits;
    hmdPosition.y = currentPosition.x * mMeterToGameUnits;
    hmdPosition.z = currentPosition.y * -mMeterToGameUnits;
    
    glm::mat4 hmdPositionMat = glm::translate(glm::mat4(1.0f), hmdPosition);
    
    // create view matrix
    glm::mat4 viewMatrix;
    if (noPosition)
    {
        viewMatrix = hmdRotationMat * glm::mat4_cast(bodyYawRotation) * bodyPositionMat;
    }
    else
    {
        viewMatrix = hmdRotationMat * hmdPositionMat * glm::mat4_cast(bodyYawRotation) * bodyPositionMat;
    }


    
    memcpy(rViewMatrix, &viewMatrix[0][0], sizeof(float) * 16);
    
    
    if (noPosition)
    {
        return true;
    }
    
    // add hmd offset to body pos

    glm::quat bodyYawRotationReverse = glm::rotate(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), (float)(DEG2RAD(bodyYaw)), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 offsetMat = glm::mat4_cast(bodyYawRotationReverse) * hmdPositionMat;
    glm::vec3 offsetPos = glm::vec3(offsetMat[3]);
    
    //Vector3f hmdPos2 = Vector3f(hmdPos.x, hmdPos.y, hmdPos.z);
    
    //Matrix4f bodyYawRotationReverse = Matrix4f::RotationZ(DEG2RAD(bodyYaw));
    //Vector3f offsetPos = (bodyYawRotationReverse * Matrix4f::Translation(hmdPos2)).GetTranslation();
    
    /// TODO: do we need this?
    offsetPos *= -1;

    xPos += offsetPos.x;
    yPos += offsetPos.y;
    zPos += offsetPos.z;

    return true;
}

bool HmdRendererOculusSdk::Get2DViewport(int& rX, int& rY, int& rW, int& rH)
{
    if (mCurrentUiMode == FULLSCREEN_MENU)
    {
        return true;
    }

    // shrink the gui for the HMD display
    float aspect = 1.0f;

    float guiScale = mGuiScale;

    rW = mRenderWidth *guiScale;
    rH = mRenderWidth *guiScale * aspect;

    rX = (mRenderWidth - rW)/2.0f;
    int xOff = mGuiOffsetFactorX > 0 ? (mRenderWidth / mGuiOffsetFactorX) : 0;
    xOff *= mEyeId == 0 ? 1 : -1;
    rX += xOff;

    rY = (mRenderHeight - rH) / 2;

    return true;
}


bool HmdRendererOculusSdk::Get2DOrtho(double &rLeft, double &rRight, double &rBottom, double &rTop, double &rZNear, double &rZFar)
{
    rLeft = 0;
    rRight = 640;
    rBottom = 480;
    rTop = 0;
    rZNear = 0;
    rZFar = 1;

    return true;
}


void HmdRendererOculusSdk::SetCurrentUiMode(UiMode mode)
{
    mCurrentUiMode = mode;

    if (mCurrentUiMode == INGAME_HUD)
    {
        mLayerMain.Header.Type = ovrLayerType_EyeFov;
        mLayerMenu.Header.Type = ovrLayerType_Disabled;
    }
    else
    {
        mLayerMain.Header.Type = ovrLayerType_Disabled;
        mLayerMenu.Header.Type = ovrLayerType_Quad;
    }
}


void HmdRendererOculusSdk::ConvertMatrix(const ovrMatrix4f& from, float* rTo)
{
    rTo[0] = from.M[0][0];
    rTo[4] = from.M[0][1];
    rTo[8] = from.M[0][2];
    rTo[12] = from.M[0][3];

    rTo[1] = from.M[1][0];
    rTo[5] = from.M[1][1];
    rTo[9] = from.M[1][2];
    rTo[13] = from.M[1][3];

    rTo[2] = from.M[2][0];
    rTo[6] = from.M[2][1];
    rTo[10] = from.M[2][2];
    rTo[14] = from.M[2][3];

    rTo[3] = from.M[3][0];
    rTo[7] = from.M[3][1];
    rTo[11] = from.M[3][2];
    rTo[15] = from.M[3][3];
}

void HmdRendererOculusSdk::PreparePlatform()
{
    // disable vsync
    // this only affects the mirror display because the oculus sdk handles vsync internally
    // we have to disable it so that the mirror display does not slow down rendering
    RenderTool::SetVSync(false);
}
