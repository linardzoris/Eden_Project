#pragma once

class CRenderTarget : public IRender_Target
{
public:
	virtual u32 get_width() override { return Device.GetSwapchainWidth(); }
	virtual u32 get_height() override { return Device.GetSwapchainHeight(); }
	virtual u32 get_target_width() override { return Device.GetSwapchainWidth(); }
	virtual u32 get_target_height() override { return Device.GetSwapchainHeight(); }
	virtual u32 get_core_width() override { return Device.GetSwapchainWidth(); }
	virtual u32 get_core_height() override { return Device.GetSwapchainHeight(); }
};
