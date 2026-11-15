// Copyright Epic Games, Inc. All Rights Reserved.

package com.epicgames.unreal.patch;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.AtomicFile;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public final class PatchActivity extends Activity
{
	private static final String TAG = "UGPPatch";
	private static final String UE_METADATA = "com.epicgames.unreal.GameActivity.";
	private static final String CDN_METADATA = "com.epicgames.unreal.patch.CdnUrl";

	private static native boolean nativeRunPatchV2(String commandLine, String cdn, String installDir,
		String internalDir, AssetManager assets, OnUpdateEventListener listener);
	private static native String nativeBuildConfigurationV2();

	public interface OnUpdateEventListener
	{
		void onUpdateEvent(int phase, int error, boolean isError, int current, int total, String message);
	}

	// libugp keeps its bootstrap state on one thread for the lifetime of this process.
	private static final HandlerThread WorkerThread = new HandlerThread("UGPPatchWorker");
	private static final Handler Worker;
	static
	{
		WorkerThread.start();
		Worker = new Handler(WorkerThread.getLooper());
	}

	// Activity changes run on the UI thread. The worker uses application data
	// and never touches a view directly.
	private static PatchActivity Current;
	private static boolean Running;
	private static boolean Ready;
	private static String HandoffToken;
	private static int Error;
	private static String Status = "Checking content version...";
	private static int Progress;
	private static int Total;
	private UGPStartupView View;
	private boolean Resumed;
	private boolean HandingOff;

	@Override
	protected void onCreate(Bundle state)
	{
		super.onCreate(state);
		Current = this;
		View = new UGPStartupView(this, this::startUpdate);
		render();
		if (!Running && !Ready && Error == 0)
		{
			startUpdate();
		}
	}

	@Override
	protected void onResume()
	{
		super.onResume();
		Resumed = true;
		render();
	}

	@Override
	protected void onPause()
	{
		Resumed = false;
		super.onPause();
	}

	@Override
	protected void onDestroy()
	{
		if (Current == this)
		{
			Current = null;
		}
		View.dismiss();
		super.onDestroy();
	}

	private void startUpdate()
	{
		if (Running || HandingOff)
		{
			return;
		}
		Running = true;
		Ready = false;
		HandoffToken = null;
		Error = 0;
		Status = "Checking content version...";
		Progress = 0;
		Total = 0;
		render();
		Worker.post(this::updateContent);
	}

	private void updateContent()
	{
		final int[] failureCode = {9000};
		final String[] failureDetail = {"The content update could not finish."};
		boolean ready;
		String token = null;
		try (UGPStartupGate lock = UGPStartupGate.tryAcquire(this))
		{
			if (lock == null)
			{
				Log.i(TAG, "Game is running; resuming without updating");
				complete(true, null, 0, "");
				return;
			}
			Bundle metadata = getPackageManager().getApplicationInfo(getPackageName(), PackageManager.GET_META_DATA).metaData;
			if (metadata == null)
			{
				throw new IOException("Missing Android application metadata");
			}
			String project = metadata.getString(UE_METADATA + "ProjectName");
			String configuration = metadata.getString(UE_METADATA + "BuildConfiguration");
			String cdn = metadata.getString(CDN_METADATA);
			if (project == null || project.isEmpty() || configuration == null || cdn == null ||
				!metadata.getBoolean(UE_METADATA + "bUseExternalFilesDir"))
			{
				throw new IOException("Missing UGP startup settings in the Android manifest");
			}
			File base = "Shipping".equals(configuration) ? getFilesDir() : getExternalFilesDir(null);
			if (base == null)
			{
				throw new IOException("Game content storage is unavailable");
			}
			File stageRoot = new File(base, "UnrealGame/" + project);
			System.loadLibrary("ugp");
			if (!("UGP_JNI_V2:" + configuration).equals(nativeBuildConfigurationV2()))
			{
				throw new IOException("The UGP library does not match the game build configuration");
			}
			UGPStartupGate.invalidate(this);
			Log.i(TAG, "Starting content update in " + stageRoot);
			ready = nativeRunPatchV2("-NoPak", cdn, stageRoot.getAbsolutePath(),
				getFilesDir().getAbsolutePath(), getAssets(), (phase, error, isError, current, total, message) ->
				{
					if (isError)
					{
						failureCode[0] = error;
						failureDetail[0] = message;
					}
					else
					{
						runOnUiThread(() -> {
							Status = message;
							Progress = phase == 3 ? current : 0;
							Total = phase == 3 ? total : 0;
							if (Current != null) Current.render();
						});
					}
				});
			if (ready)
			{
				installBootstrapAssets(stageRoot, project);
				token = UGPStartupGate.markReady(this);
			}
		}
		catch (Exception | LinkageError error)
		{
			Log.e(TAG, "Could not prepare game startup", error);
			complete(false, null, 9003, error.getMessage());
			return;
		}
		// The updater lock has been released before the game can acquire it.
		complete(ready, token, failureCode[0], failureDetail[0]);
	}

	private void installBootstrapAssets(File stageRoot, String project) throws IOException
	{
		File projectDir = new File(stageRoot, project);
		copyBootstrapAsset("UGPProject.uproject", new File(projectDir, project + ".uproject"));
		copyBootstrapAsset("UECommandLine.txt", new File(stageRoot, "UECommandLine.txt"));
	}

	private void copyBootstrapAsset(String name, File destination) throws IOException
	{
		AtomicFile target = new AtomicFile(destination);
		try (InputStream input = getAssets().open(name))
		{
			FileOutputStream output = target.startWrite();
			try
			{
				byte[] buffer = new byte[8192];
				for (int count; (count = input.read(buffer)) != -1;)
				{
					output.write(buffer, 0, count);
				}
				target.finishWrite(output);
			}
			catch (IOException error)
			{
				target.failWrite(output);
				throw error;
			}
		}
	}

	private void complete(boolean ready, String token, int error, String message)
	{
		runOnUiThread(() -> {
			Running = false;
			Ready = ready;
			HandoffToken = token;
			Error = ready ? 0 : error;
			Status = message;
			if (Current != null) Current.render();
		});
	}

	private void render()
	{
		if (HandingOff)
		{
			return;
		}
		if (Ready && Resumed)
		{
			HandingOff = true;
			Ready = false;
			Intent launch = new Intent();
			launch.setClassName(this, "com.epicgames.unreal.GameActivity");
			UGPStartupGate.handOff(launch, HandoffToken);
			launch.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
			Log.i(TAG, "Starting GameActivity");
			startActivity(launch);
			finish();
		}
		else if (Error != 0)
		{
			View.failure(Error, Status);
		}
		else
		{
			View.status(Status, Progress, Total);
		}
	}
}
