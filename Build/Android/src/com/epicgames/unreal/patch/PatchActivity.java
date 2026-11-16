// Copyright Epic Games, Inc. All Rights Reserved.

package com.epicgames.unreal.patch;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;

public final class PatchActivity extends Activity
{
	private static final int NOTIFICATION_PERMISSION_REQUEST = 1;
	private final Runnable Render = this::render;
	private UGPStartupView View;
	private boolean Resumed;
	private boolean HandingOff;
	private boolean AwaitingNotificationPermission;
	private boolean StartAfterPermission;

	@Override
	protected void onCreate(Bundle state)
	{
		super.onCreate(state);
		AwaitingNotificationPermission = state != null && state.getBoolean("awaiting_notification_permission");
		StartAfterPermission = state != null && state.getBoolean("start_after_permission");
		View = new UGPStartupView(this, this::requestUpdate);
		UGPUpdate.OnChanged = Render;
		render();
	}

	@Override
	protected void onResume()
	{
		super.onResume();
		Resumed = true;
		if (!HandingOff) UGPUpdate.check(this);
		if (StartAfterPermission)
		{
			StartAfterPermission = false;
			UGPUpdate.request(this);
		}
		render();
	}

	@Override
	protected void onPause()
	{
		Resumed = false;
		super.onPause();
	}

	@Override
	protected void onSaveInstanceState(Bundle state)
	{
		state.putBoolean("awaiting_notification_permission", AwaitingNotificationPermission);
		state.putBoolean("start_after_permission", StartAfterPermission);
		super.onSaveInstanceState(state);
	}

	@Override
	protected void onDestroy()
	{
		if (UGPUpdate.OnChanged == Render) UGPUpdate.OnChanged = null;
		View.dismiss();
		super.onDestroy();
	}

	private void requestUpdate()
	{
		if (AwaitingNotificationPermission) return;
		if (!UGPUpdate.Version.isEmpty() && Build.VERSION.SDK_INT >= 33 &&
			checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
		{
			SharedPreferences preferences = getSharedPreferences("ugp", MODE_PRIVATE);
			if (!preferences.getBoolean("notifications_requested", false))
			{
				preferences.edit().putBoolean("notifications_requested", true).apply();
				AwaitingNotificationPermission = true;
				requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, NOTIFICATION_PERMISSION_REQUEST);
				return;
			}
		}
		UGPUpdate.request(this);
	}

	@Override
	public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults)
	{
		super.onRequestPermissionsResult(requestCode, permissions, grantResults);
		if (requestCode == NOTIFICATION_PERMISSION_REQUEST && AwaitingNotificationPermission)
		{
			AwaitingNotificationPermission = false;
			if (grantResults.length > 0)
			{
				if (Resumed) UGPUpdate.request(this);
				else StartAfterPermission = true;
			}
			else
			{
				render();
			}
		}
	}

	private void render()
	{
		if (HandingOff)
		{
			return;
		}
		if (UGPUpdate.State == UGPUpdate.Phase.READY && Resumed)
		{
			HandingOff = true;
			Intent launch = new Intent();
			launch.setClassName(this, "com.epicgames.unreal.GameActivity");
			UGPStartupGate.handOff(launch, UGPUpdate.HandoffToken);
			UGPUpdate.State = UGPUpdate.Phase.IDLE;
			UGPUpdate.HandoffToken = null;
			launch.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
			Log.i(UGPUpdate.TAG, "Starting GameActivity");
			startActivity(launch);
			finish();
		}
		else if (UGPUpdate.State == UGPUpdate.Phase.OFFER)
		{
			View.offer(UGPUpdate.Status);
		}
		else if (UGPUpdate.State == UGPUpdate.Phase.FAILED)
		{
			View.failure(UGPUpdate.Error, UGPUpdate.Status, UGPUpdate.FailedRetries);
		}
		else
		{
			String status = UGPUpdate.Retrying ? "Retry " + (UGPUpdate.FailedRetries + 1) + ": " + UGPUpdate.Status : UGPUpdate.Status;
			View.status(status, UGPUpdate.Progress, UGPUpdate.Total);
		}
	}
}
