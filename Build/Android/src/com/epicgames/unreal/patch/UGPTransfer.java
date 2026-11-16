// Copyright Epic Games, Inc. All Rights Reserved.

package com.epicgames.unreal.patch;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PersistableBundle;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;

/** Android scheduling and notifications for a single confirmed content version. */
public final class UGPTransfer
{
	private static final int JOB_ID = 0x554750;
	private static final int NOTIFICATION_ID = 0x554751;
	private static final String CHANNEL = "ugp-content-updates";
	private static final String VERSION = "content_version";

	static String approvedVersion(Context context)
	{
		return context.getSharedPreferences("ugp-transfer", Context.MODE_PRIVATE).getString(VERSION, null);
	}

	static void clearApproval(Context context, String version)
	{
		if (version.equals(approvedVersion(context)))
		{
			context.getSharedPreferences("ugp-transfer", Context.MODE_PRIVATE).edit().remove(VERSION).apply();
		}
	}

	static boolean schedule(Context context, String version)
	{
		// The Task Manager may kill this process without calling onStopJob.
		if (!context.getSharedPreferences("ugp-transfer", Context.MODE_PRIVATE).edit().putString(VERSION, version).commit())
		{
			return false;
		}
		if (Build.VERSION.SDK_INT >= 34)
		{
			PersistableBundle extras = new PersistableBundle();
			extras.putString(VERSION, version);
			JobInfo job = new JobInfo.Builder(JOB_ID, new ComponentName(context, UserTransfer.class))
				.setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
				.setUserInitiated(true)
				.setExtras(extras)
				.build();
			return context.getSystemService(JobScheduler.class).schedule(job) == JobScheduler.RESULT_SUCCESS;
		}
		return context.startForegroundService(new Intent(context, ForegroundTransfer.class).putExtra(VERSION, version)) != null;
	}

	static String pendingVersion(Context context)
	{
		if (Build.VERSION.SDK_INT >= 34)
		{
			JobInfo job = context.getSystemService(JobScheduler.class).getPendingJob(JOB_ID);
			if (job != null)
			{
				return job.getExtras().getString(VERSION);
			}
		}
		return null;
	}

	private static final class ProgressNotification
	{
		private final NotificationManager Manager;
		private final Notification.Builder Builder;
		private long LastUpdate;

		ProgressNotification(Context context)
		{
			Manager = context.getSystemService(NotificationManager.class);
			Manager.createNotificationChannel(new NotificationChannel(CHANNEL, "Game content updates", NotificationManager.IMPORTANCE_LOW));
			Intent open = new Intent(context, PatchActivity.class)
				.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
			Builder = new Notification.Builder(context, CHANNEL)
				.setSmallIcon(android.R.drawable.stat_sys_download)
				.setContentTitle("IOStoreDemo Content Update")
				.setContentText("Starting download...")
				.setOngoing(true)
				.setOnlyAlertOnce(true)
				.setProgress(0, 0, true)
				.setContentIntent(PendingIntent.getActivity(context, 0, open, PendingIntent.FLAG_IMMUTABLE));
		}

		Notification initial()
		{
			return Builder.build();
		}

		void update()
		{
			long now = SystemClock.elapsedRealtime();
			if (now - LastUpdate < 1000)
			{
				return;
			}
			LastUpdate = now;
			Manager.notify(NOTIFICATION_ID, Builder.setContentText(UGPUpdate.Status)
				.setProgress(UGPUpdate.Total, UGPUpdate.Progress, UGPUpdate.Total == 0)
				.build());
		}
	}

	public static final class UserTransfer extends JobService
	{
		private JobParameters Job;
		private UGPUpdate.Attempt Attempt;

		@Override
		public boolean onStartJob(JobParameters params)
		{
			String version = params.getExtras().getString(VERSION, "");
			if (version.isEmpty() || Job != null || !version.equals(approvedVersion(this)))
			{
				return false;
			}
			ProgressNotification notification = new ProgressNotification(this);
			setNotification(params, NOTIFICATION_ID, notification.initial(), JOB_END_NOTIFICATION_POLICY_REMOVE);
			Job = params;
			Attempt = UGPUpdate.begin(this, version, notification::update, () -> {
				if (Job == params)
				{
					Job = null;
					Attempt = null;
					jobFinished(params, false);
				}
			});
			if (Attempt == null)
			{
				Job = null;
				return false;
			}
			return true;
		}

		@Override
		public boolean onStopJob(JobParameters params)
		{
			if (Job == null || Job.getJobId() != params.getJobId())
			{
				return false;
			}
			int reason = params.getStopReason();
			boolean reschedule = reason != JobParameters.STOP_REASON_CANCELLED_BY_APP &&
				reason != JobParameters.STOP_REASON_USER;
			Log.i(UGPUpdate.TAG, "Stopping scheduled update, reason=" + reason + ", reschedule=" + reschedule);
			Job = null;
			UGPUpdate.stop(Attempt, reschedule);
			Attempt = null;
			return reschedule;
		}
	}

	public static final class ForegroundTransfer extends Service
	{
		private UGPUpdate.Attempt Attempt;
		private PowerManager.WakeLock WakeLock;

		@Override
		public int onStartCommand(Intent intent, int flags, int startId)
		{
			if (Attempt != null)
			{
				return START_NOT_STICKY;
			}
			String version = intent == null ? null : intent.getStringExtra(VERSION);
			if (version == null || version.isEmpty() || !version.equals(approvedVersion(this)))
			{
				stopSelf();
				return START_NOT_STICKY;
			}
			ProgressNotification notification = new ProgressNotification(this);
			startForeground(NOTIFICATION_ID, notification.initial(), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
			WakeLock = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "IOStoreDemo:UGPTransfer");
			WakeLock.acquire();
			Attempt = UGPUpdate.begin(this, version, notification::update, () -> {
				Attempt = null;
				stopSelf();
			});
			if (Attempt == null) stopSelf();
			return START_NOT_STICKY;
		}

		@Override
		public void onDestroy()
		{
			UGPUpdate.stop(Attempt, false);
			if (WakeLock != null && WakeLock.isHeld()) WakeLock.release();
			super.onDestroy();
		}

		@Override
		public IBinder onBind(Intent intent)
		{
			return null;
		}
	}
}
