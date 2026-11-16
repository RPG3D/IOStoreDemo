// Copyright Epic Games, Inc. All Rights Reserved.

package com.epicgames.unreal.patch;

import android.app.Activity;
import android.app.AlertDialog;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import java.util.Locale;

public final class UGPStartupView
{
	private final Activity Activity;
	private final Runnable Retry;
	private FrameLayout Overlay;
	private TextView Status;
	private ProgressBar Spinner;
	private ProgressBar Bar;
	private Button Download;
	private AlertDialog Failure;

	public UGPStartupView(Activity activity, Runnable retry)
	{
		Activity = activity;
		Retry = retry;
		attach();
	}

	private void attach()
	{
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
		Download = new Button(Activity);
		Download.setText("Download update");
		Download.setVisibility(View.GONE);
		Download.setOnClickListener(view -> Retry.run());
		bottom.addView(Spinner, new LinearLayout.LayoutParams(-2, -2));
		bottom.addView(Bar, new LinearLayout.LayoutParams(-1, -2));
		bottom.addView(Status, new LinearLayout.LayoutParams(-2, -2));
		bottom.addView(Download, new LinearLayout.LayoutParams(-2, -2));
		Overlay.addView(bottom, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
		Activity.addContentView(Overlay, new ViewGroup.LayoutParams(-1, -1));
	}

	public void status(String text, int current, int total)
	{
		Activity.runOnUiThread(() -> {
			dismissFailure();
			boolean downloading = total > 0;
			Status.setText(text);
			Download.setVisibility(View.GONE);
			Spinner.setVisibility(downloading ? View.GONE : View.VISIBLE);
			Bar.setVisibility(downloading ? View.VISIBLE : View.GONE);
			if (downloading)
			{
				Bar.setProgress((int) (1000L * current / total));
			}
		});
	}

	public void offer(String text)
	{
		dismissFailure();
		Status.setText(text);
		Spinner.setVisibility(View.GONE);
		Bar.setVisibility(View.GONE);
		Download.setVisibility(View.VISIBLE);
	}

	public void failure(int code, String detail, int failedRetries)
	{
		Activity.runOnUiThread(() -> {
			if (Failure != null) return;
			Spinner.setVisibility(View.GONE);
			Bar.setVisibility(View.GONE);
			Download.setVisibility(View.GONE);
			Failure = new AlertDialog.Builder(Activity)
				.setTitle("Content Update Failed")
				.setMessage(String.format(Locale.ROOT, "UGP-%04d\n%s\n\nFailed retries: %d", code, detail, failedRetries))
				.setCancelable(false)
				.setPositiveButton("Retry", (dialog, button) -> {
					Failure = null;
					Retry.run();
				})
				.show();
		});
	}

	public void dismiss()
	{
		Activity.runOnUiThread(() -> {
			dismissFailure();
			if (Overlay.getParent() instanceof ViewGroup) { ((ViewGroup) Overlay.getParent()).removeView(Overlay); }
		});
	}

	private void dismissFailure()
	{
		if (Failure != null)
		{
			Failure.dismiss();
			Failure = null;
		}
	}

}
