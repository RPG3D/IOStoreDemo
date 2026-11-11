// Copyright Epic Games, Inc. All Rights Reserved.

package com.epicgames.unreal.patch;

import android.app.AlertDialog;
import com.epicgames.unreal.GameActivity;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Locale;
import java.util.concurrent.Semaphore;

public final class UGPStartupView
{
	private static final int MAX_BODY_BYTES = 64 << 20;
	private final GameActivity Activity;
	private final Semaphore Retry = new Semaphore(0);
	private FrameLayout Overlay;
	private TextView Status;
	private ProgressBar Spinner;
	private ProgressBar Bar;
	private AlertDialog Failure;
	private int FailedRetries;
	private boolean Retrying;

	public UGPStartupView(GameActivity activity)
	{
		Activity = activity;
		Activity.runOnUiThread(this::attach);
	}

	private void attach()
	{
		Activity.AndroidThunkJava_DismissSplashScreen();
		Overlay = new FrameLayout(Activity);
		Overlay.setBackgroundColor(0xff181a20);
		Overlay.setClickable(true);
		Overlay.setFitsSystemWindows(true);

		TextView title = new TextView(Activity);
		title.setText("IOStoreDemo Content Update");
		title.setTextColor(0xffffffff);
		title.setTextSize(22);
		title.setGravity(Gravity.CENTER);
		Overlay.addView(title, new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER));

		LinearLayout bottom = new LinearLayout(Activity);
		bottom.setOrientation(LinearLayout.VERTICAL);
		bottom.setGravity(Gravity.CENTER_HORIZONTAL);
		int margin = Math.round(24 * Activity.getResources().getDisplayMetrics().density);
		bottom.setPadding(margin, margin, margin, margin);
		Spinner = new ProgressBar(Activity);
		Bar = new ProgressBar(Activity, null, android.R.attr.progressBarStyleHorizontal);
		Bar.setMax(1000);
		Bar.setVisibility(View.GONE);
		Status = new TextView(Activity);
		Status.setTextColor(0xffffffff);
		Status.setText("Checking content version...");
		bottom.addView(Spinner, new LinearLayout.LayoutParams(-2, -2));
		bottom.addView(Bar, new LinearLayout.LayoutParams(-1, -2));
		bottom.addView(Status, new LinearLayout.LayoutParams(-2, -2));
		Overlay.addView(bottom, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
		Activity.addContentView(Overlay, new ViewGroup.LayoutParams(-1, -1));
	}

	public void status(String text, int current, int total)
	{
		Activity.runOnUiThread(() -> {
			boolean downloading = total > 0;
			Status.setText(Retrying ? "Retry " + (FailedRetries + 1) + ": " + text : text);
			Spinner.setVisibility(downloading ? View.GONE : View.VISIBLE);
			Bar.setVisibility(downloading ? View.VISIBLE : View.GONE);
			if (downloading)
			{
				Bar.setProgress((int) (1000L * current / total));
			}
		});
	}

	public void failure(int code, String detail)
	{
		Activity.runOnUiThread(() -> {
			if (Retrying)
			{
				++FailedRetries;
			}
			Retrying = false;
			Spinner.setVisibility(View.GONE);
			Bar.setVisibility(View.GONE);
			Failure = new AlertDialog.Builder(Activity)
				.setTitle("Content Update Failed")
				.setMessage(String.format(Locale.ROOT, "UGP-%04d\n%s\n\nFailed retries: %d", code, detail, FailedRetries))
				.setCancelable(false)
				.setPositiveButton("Retry", (dialog, button) -> {
					Failure = null;
					Retrying = true;
					status("Checking content version...", 0, 0);
					Retry.release();
				})
				.show();
		});
	}

	public void awaitRetry()
	{
		Retry.acquireUninterruptibly();
	}

	public void dismiss()
	{
		Activity.runOnUiThread(() -> {
			if (Failure != null)
			{
				Failure.dismiss();
			}
			((ViewGroup) Overlay.getParent()).removeView(Overlay);
		});
	}

	// The game HTTP module needs engine configuration, which is loaded after
	// Pak mount. Use Android networking while the game thread is at startup.
	public static byte[] download(String resource, long offset, long size) throws IOException
	{
		if (offset < 0 || size == 0 || size > MAX_BODY_BYTES || (size > 0 && offset > Long.MAX_VALUE - size))
		{
			throw new IOException("Invalid content range");
		}

		HttpURLConnection connection = (HttpURLConnection) new URL(resource).openConnection();
		try
		{
			boolean range = offset > 0 || size >= 0;
			connection.setConnectTimeout(15000);
			connection.setReadTimeout(15000);
			connection.setRequestProperty("Accept-Encoding", "identity");
			if (range)
			{
				String end = size >= 0 ? Long.toString(offset + size - 1) : "";
				connection.setRequestProperty("Range", "bytes=" + offset + "-" + end);
			}
			int code = connection.getResponseCode();
			if (code != (range ? 206 : 200))
			{
				throw new IOException("Unexpected HTTP status " + code);
			}
			if (range)
			{
				String header = connection.getHeaderField("Content-Range");
				String expected = size >= 0 ? "bytes " + offset + "-" + (offset + size - 1) + "/" : "bytes " + offset + "-";
				if (header == null || !header.startsWith(expected))
				{
					throw new IOException("Invalid Content-Range");
				}
			}
			long advertised = connection.getContentLengthLong();
			int limit = size >= 0 ? (int) size : MAX_BODY_BYTES;
			if (advertised > limit || (size >= 0 && advertised >= 0 && advertised != size))
			{
				throw new IOException("Invalid content length");
			}

			try (InputStream input = connection.getInputStream();
				ByteArrayOutputStream bytes = new ByteArrayOutputStream(advertised > 0 ? (int) advertised : 8192))
			{
				byte[] buffer = new byte[32768];
				for (int count; (count = input.read(buffer)) != -1;)
				{
					if (count > limit - bytes.size())
					{
						throw new IOException("Content exceeds its byte limit");
					}
					bytes.write(buffer, 0, count);
				}
				if (bytes.size() == 0 || (size >= 0 && bytes.size() != size) ||
					(advertised >= 0 && bytes.size() != advertised))
				{
					throw new IOException("Content download was interrupted");
				}
				return bytes.toByteArray();
			}
		}
		finally
		{
			connection.disconnect();
		}
	}
}
