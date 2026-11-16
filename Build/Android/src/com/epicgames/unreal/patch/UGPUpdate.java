// Copyright Epic Games, Inc. All Rights Reserved.

package com.epicgames.unreal.patch;

import android.content.Context;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.AtomicFile;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.concurrent.atomic.AtomicBoolean;

/** One update worker shared by the activity and both background entry points. */
final class UGPUpdate
{
	enum Phase { IDLE, CHECKING, OFFER, QUEUED, TRANSFERRING, FAILED, READY }

	static final String TAG = "UGPPatch";
	static final String CHECKING_VERSION = "Checking content version...";
	private static final String UE_METADATA = "com.epicgames.unreal.GameActivity.";
	private static final String CDN_METADATA = "com.epicgames.unreal.patch.CdnUrl";
	private static final int DOWNLOAD_PHASE = 3; // EUpdatePhase::Download
	private static final Handler Main = new Handler(Looper.getMainLooper());
	private static final Handler Worker;
	static
	{
		HandlerThread thread = new HandlerThread("UGPPatchWorker");
		thread.start();
		Worker = new Handler(thread.getLooper());
	}

	private static native String nativeRunPatchV4(String commandLine, String cdn, String installDir,
		String internalDir, AssetManager assets, String version, boolean installedOnly,
		AtomicBoolean cancelled, OnUpdateEventListener listener);
	private static native String nativeBuildConfigurationV4();

	interface OnUpdateEventListener
	{
		void onUpdateEvent(int phase, int error, boolean isError, int current, int total, String message);
	}

	static final class Attempt
	{
		final String Version;
		final boolean InstalledOnly;
		final AtomicBoolean Cancelled = new AtomicBoolean();

		Attempt(String version, boolean installedOnly)
		{
			Version = version;
			InstalledOnly = installedOnly;
		}
	}

	// Read and written on the main thread. An empty version requests a preflight.
	static Phase State = Phase.IDLE;
	static String Version = "";
	static String HandoffToken;
	static String Status = CHECKING_VERSION;
	static int Error;
	static int Progress;
	static int Total;
	static int FailedRetries;
	static boolean Retrying;
	static Runnable OnChanged;
	private static Attempt Active;

	static void check(Context context)
	{
		final boolean offline = !hasNetwork(context);
		if (State != Phase.IDLE && !(State == Phase.QUEUED && offline))
		{
			return;
		}
		FailedRetries = 0;
		Retrying = false;
		checkForUpdate(context, offline);
	}

	private static boolean hasNetwork(Context context)
	{
		ConnectivityManager manager = context.getSystemService(ConnectivityManager.class);
		Network network = manager.getActiveNetwork();
		NetworkCapabilities capabilities = network == null ? null : manager.getNetworkCapabilities(network);
		return capabilities != null && capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
	}

	private static void checkForUpdate(Context context, boolean offline)
	{
		String pending = offline ? null : UGPTransfer.pendingVersion(context);
		if (pending != null)
		{
			waitForTransfer(pending, "Waiting to continue the content update...");
			return;
		}
		begin(context, "", offline, null, null);
	}

	static void request(Context context)
	{
		if (State != Phase.OFFER && State != Phase.FAILED)
		{
			return;
		}
		Retrying = State == Phase.FAILED;
		final boolean offline = !hasNetwork(context);
		if (offline || Version.isEmpty())
		{
			checkForUpdate(context, offline);
			return;
		}

		waitForTransfer(Version, "Starting download...");
		try
		{
			if (!UGPTransfer.schedule(context, Version))
			{
				fail(9003, "Android could not schedule the download. Return to the update screen and retry.");
			}
		}
		catch (RuntimeException error)
		{
			Log.e(TAG, "Could not schedule update", error);
			fail(9003, "Android could not start the background download.");
		}
		changed();
	}

	static Attempt begin(Context context, String version, Runnable onProgress, Runnable onFinished)
	{
		return begin(context, version, false, onProgress, onFinished);
	}

	private static Attempt begin(Context context, String version, boolean installedOnly, Runnable onProgress, Runnable onFinished)
	{
		if (Active != null)
		{
			if (!Active.Version.isEmpty() || version.isEmpty())
			{
				return null;
			}
			Active.Cancelled.set(true); // A restored job takes precedence over preflight.
		}
		Attempt attempt = new Attempt(version, installedOnly);
		Active = attempt;
		Version = version;
		State = version.isEmpty() ? Phase.CHECKING : Phase.TRANSFERRING;
		Status = CHECKING_VERSION;
		Error = Progress = Total = 0;
		changed();
		Context app = context.getApplicationContext();
		Worker.post(() -> run(app, attempt, onProgress, onFinished));
		return attempt;
	}

	static void stop(Attempt attempt, boolean rescheduled)
	{
		if (attempt == null || Active != attempt)
		{
			return;
		}
		attempt.Cancelled.set(true);
		Active = null;
		Retrying = false;
		waitForTransfer(attempt.Version, "Download paused.");
		if (!rescheduled)
		{
			State = Phase.OFFER;
			changed();
		}
	}

	private static void waitForTransfer(String version, String status)
	{
		Version = version;
		State = Phase.QUEUED;
		Status = status;
		Error = Progress = Total = 0;
		changed();
	}

	private static void run(Context context, Attempt attempt, Runnable onProgress, Runnable onFinished)
	{
		final int[] failureCode = {0};
		final String[] failureDetail = {"The content update could not finish."};
		String nextVersion = null;
		String token = null;
		try
		{
			if (!attempt.Cancelled.get())
			{
				String approved = attempt.Version.isEmpty() && !attempt.InstalledOnly ? UGPTransfer.approvedVersion(context) : null;
				try (UGPStartupGate lock = UGPStartupGate.tryAcquire(context))
				{
					if (lock == null)
					{
						Log.i(TAG, "Game is running; resuming without updating");
						nextVersion = "";
					}
					else if (approved != null)
					{
						nextVersion = approved;
					}
					else
					{
						Bundle metadata = context.getPackageManager()
							.getApplicationInfo(context.getPackageName(), PackageManager.GET_META_DATA).metaData;
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
						File base = "Shipping".equals(configuration) ? context.getFilesDir() : context.getExternalFilesDir(null);
						if (base == null)
						{
							throw new IOException("Game content storage is unavailable");
						}
						File stageRoot = new File(base, "UnrealGame/" + project);
						System.loadLibrary("ugp");
							if (!("UGP_JNI_V4:" + configuration + ":Saved/Paks").equals(nativeBuildConfigurationV4()))
						{
							throw new IOException("The UGP library does not match the game build configuration or content layout");
						}
						UGPStartupGate.invalidate(context);
						Log.i(TAG, (attempt.Version.isEmpty() ? "Checking content in " : "Updating content in ") + stageRoot);
							nextVersion = nativeRunPatchV4("-NoPak", cdn, stageRoot.getAbsolutePath(),
								context.getFilesDir().getAbsolutePath(), context.getAssets(), attempt.Version, attempt.InstalledOnly, attempt.Cancelled,
							(phase, error, isError, current, total, message) -> {
								if (isError)
								{
									failureCode[0] = error;
									failureDetail[0] = message;
								}
								else
								{
									Main.post(() -> {
										if (Active != attempt) return;
										Status = message;
										Progress = phase == DOWNLOAD_PHASE ? current : 0;
										Total = phase == DOWNLOAD_PHASE ? total : 0;
										changed();
										if (onProgress != null) onProgress.run();
									});
								}
							});
						if ("".equals(nextVersion) && !attempt.Cancelled.get())
						{
							copyBootstrapAsset(context, "UGPProject.uproject", new File(stageRoot, project + "/" + project + ".uproject"));
							copyBootstrapAsset(context, "UECommandLine.txt", new File(stageRoot, "UECommandLine.txt"));
							token = UGPStartupGate.markReady(context);
						}
					}
				}
			}
		}
		catch (Exception | LinkageError error)
		{
			Log.e(TAG, "Could not prepare game startup", error);
			nextVersion = null;
			failureCode[0] = 9003;
			failureDetail[0] = error.getMessage();
		}

		String result = nextVersion;
		String handoff = token;
		Main.post(() -> {
			if (Active == attempt)
			{
				Active = null;
				if (result == null)
				{
					fail(failureCode[0] == 0 ? 9000 : failureCode[0], failureDetail[0]);
				}
				else
				{
					if (!attempt.Version.isEmpty() &&
						((result.isEmpty() && handoff != null && failureCode[0] == 0) ||
							(!result.isEmpty() && !result.equals(attempt.Version))))
					{
						UGPTransfer.clearApproval(context, attempt.Version);
					}
					Retrying = false;
					Version = result;
					HandoffToken = handoff;
					State = result.isEmpty() ? Phase.READY : Phase.OFFER;
					Status = result.isEmpty() ? "Content ready." :
						(result.equals(UGPTransfer.approvedVersion(context)) ? "Continue downloading the selected update." : "An update is available.");
					Progress = Total = 0;
				}
				changed();
			}
			if (onFinished != null) onFinished.run();
		});
	}

	private static void fail(int error, String message)
	{
		if (Retrying) ++FailedRetries;
		Retrying = false;
		State = Phase.FAILED;
		Error = error;
		Status = message;
	}

	private static void changed()
	{
		if (OnChanged != null) OnChanged.run();
	}

	private static void copyBootstrapAsset(Context context, String name, File destination) throws IOException
	{
		AtomicFile target = new AtomicFile(destination);
		try (InputStream input = context.getAssets().open(name))
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
}
