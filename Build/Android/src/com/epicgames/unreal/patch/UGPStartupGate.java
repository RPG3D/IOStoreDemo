// Copyright Epic Games, Inc. All Rights Reserved.

package com.epicgames.unreal.patch;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.util.AtomicFile;
import android.util.Log;
import java.io.Closeable;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.charset.StandardCharsets;

/** Coordinates the updater and the game in app-private storage. */
public final class UGPStartupGate implements Closeable
{
	private static final String TAG = "UGPStartupGate";
	private static final String HANDOFF = "com.epicgames.unreal.patch.Handoff";
	private static UGPStartupGate GameLock;
	private final RandomAccessFile FileHandle;
	private final FileLock Lock;

	private UGPStartupGate(RandomAccessFile file, FileLock lock)
	{
		FileHandle = file;
		Lock = lock;
	}

	static UGPStartupGate tryAcquire(Activity activity) throws IOException
	{
		RandomAccessFile file = new RandomAccessFile(new File(activity.getFilesDir(), "ugp-startup.lock"), "rw");
		boolean acquired = false;
		try
		{
			FileLock lock = file.getChannel().tryLock();
			acquired = lock != null;
			return acquired ? new UGPStartupGate(file, lock) : null;
		}
		catch (OverlappingFileLockException busy)
		{
			return null;
		}
		finally
		{
			if (!acquired)
			{
				file.close();
			}
		}
	}

	@Override
	public void close() throws IOException
	{
		try
		{
			Lock.release();
		}
		finally
		{
			FileHandle.close();
		}
	}

	private static AtomicFile readyFile(Activity activity)
	{
		return new AtomicFile(new File(activity.getFilesDir(), "ugp-content-ready"));
	}

	private static String apkStamp(Activity activity) throws IOException
	{
		try
		{
			return Long.toString(activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).lastUpdateTime);
		}
		catch (PackageManager.NameNotFoundException error)
		{
			throw new IOException(error);
		}
	}

	static void invalidate(Activity activity) throws IOException
	{
		AtomicFile ready = readyFile(activity);
		ready.delete();
		File base = ready.getBaseFile();
		if (base.exists() || new File(base.getPath() + ".bak").exists() || new File(base.getPath() + ".new").exists())
		{
			throw new IOException("Could not clear the content readiness record");
		}
	}

	static String markReady(Activity activity) throws IOException
	{
		String token = apkStamp(activity) + ":" + java.util.UUID.randomUUID();
		byte[] stamp = token.getBytes(StandardCharsets.UTF_8);
		AtomicFile ready = readyFile(activity);
		FileOutputStream output = ready.startWrite();
		try
		{
			output.write(stamp);
			ready.finishWrite(output);
			return token;
		}
		catch (IOException error)
		{
			ready.failWrite(output);
			throw error;
		}
	}

	static void handOff(Intent intent, String token)
	{
		if (token != null)
		{
			intent.putExtra(HANDOFF, token);
		}
	}

	/** Called by UPL before GameActivity starts the system info thread or loads Unreal. */
	public static boolean enterGame(Activity activity)
	{
		try
		{
			if (GameLock != null)
			{
				// GameActivity is singleTask. A live instance is resumed via onNewIntent.
				return true;
			}
			UGPStartupGate lock = tryAcquire(activity);
			if (lock != null)
			{
				try
				{
					String stamp = new String(readyFile(activity).readFully(), StandardCharsets.UTF_8);
					if (stamp.startsWith(apkStamp(activity) + ":") && stamp.equals(activity.getIntent().getStringExtra(HANDOFF)))
					{
						invalidate(activity);
						GameLock = lock;
						Log.i(TAG, "Content ready before Unreal library load");
						return true;
					}
				}
				finally
				{
					if (GameLock == null)
					{
						lock.close();
					}
				}
			}
		}
		catch (IOException error)
		{
			Log.i(TAG, "Content readiness check failed", error);
		}
		// Android can restore GameActivity from Recents without the launcher.
		// End this cold game process before NativeActivity can load Unreal.
		Log.i(TAG, "Redirecting cold game launch to PatchActivity");
		Intent patch = new Intent(activity, PatchActivity.class);
		patch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
		activity.startActivity(patch);
		android.os.Process.killProcess(android.os.Process.myPid());
		return false;
	}
}
