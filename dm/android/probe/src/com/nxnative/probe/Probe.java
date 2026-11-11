// Shows the native probe's report (jni/probe.c) with Share and Copy buttons so it can be sent back.
package com.nxnative.probe;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

public class Probe extends Activity {
    static { System.loadLibrary("probe"); }
    static native String run();

    // What only Java can see (the engine's GameActivity uses the same calls): the screen's modes (it picks the lowest of at least 60 Hz the game's frames divide),
    // sustained performance (offered on some phones), the Android version
    private String phone() {
        final String NL = "\n";
        StringBuilder r = new StringBuilder(NL + "== Phone (Java side)" + NL);
        r.append("  Android API ").append(android.os.Build.VERSION.SDK_INT).append(", ").append(android.os.Build.MANUFACTURER).append(" ").append(android.os.Build.MODEL).append(NL);
        try {
            android.view.Display d = android.os.Build.VERSION.SDK_INT >= 30 ? getDisplay() : getWindowManager().getDefaultDisplay();
            android.view.Display.Mode cur = d.getMode(); r.append("  screen modes:");
            int best30 = 0, best60 = 0;
            for (android.view.Display.Mode m : d.getSupportedModes()) {
                r.append(String.format(java.util.Locale.ROOT, " %dx%d@%.1f%s", m.getPhysicalWidth(), m.getPhysicalHeight(), m.getRefreshRate(), m.getModeId() == cur.getModeId() ? "*" : ""));
                if (m.getPhysicalWidth() != cur.getPhysicalWidth() || m.getPhysicalHeight() != cur.getPhysicalHeight()) continue;
                int hz = Math.round(m.getRefreshRate());
                if (hz >= 60 && hz % 30 == 0 && (best30 == 0 || hz < best30)) best30 = hz;
                if (hz >= 60 && hz % 60 == 0 && (best60 == 0 || hz < best60)) best60 = hz;
            }
            r.append(NL).append("  [INFO] screen-rate: at 30 fps the engine asks for ").append(best30 > 0 ? best30 + " Hz" : "the phone's own rate (no mode divides evenly)")
             .append(", at 60 fps for ").append(best60 > 0 ? best60 + " Hz" : "the phone's own rate").append(" (now ").append(Math.round(cur.getRefreshRate())).append(" Hz)").append(NL);
        } catch (RuntimeException e) { r.append("  screen modes: not readable (").append(e).append(")").append(NL); }
        try { android.os.PowerManager pm = getSystemService(android.os.PowerManager.class);
              r.append("  [INFO] sustained-performance: ").append(pm != null && pm.isSustainedPerformanceModeSupported() ? "offered (the Sustained performance setting shows)" : "not offered on this phone").append(NL);
        } catch (RuntimeException e) { }
        r.append("  [INFO] loading-hint: ").append(android.os.Build.VERSION.SDK_INT >= 33 ? "Android 13+: the engine tells the system when the game loads" : "Android before 13: not available").append(NL);
        return r.toString();
    }
    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        final String report = run() + phone();
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL); root.setFitsSystemWindows(true);
        LinearLayout bar = new LinearLayout(this);
        Button share = new Button(this); share.setText("Share report");
        share.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_SEND); i.setType("text/plain");
            i.putExtra(Intent.EXTRA_SUBJECT, "nxnative probe"); i.putExtra(Intent.EXTRA_TEXT, report);
            startActivity(Intent.createChooser(i, "Share report"));
        });
        Button copy = new Button(this); copy.setText("Copy");
        copy.setOnClickListener(v -> {
            ((ClipboardManager) getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("nxnative probe", report));
            Toast.makeText(this, "Copied", Toast.LENGTH_SHORT).show();
        });
        bar.addView(share); bar.addView(copy); root.addView(bar);
        TextView t = new TextView(this); t.setText(report); t.setTypeface(Typeface.MONOSPACE); t.setTextSize(11); t.setTextIsSelectable(true); t.setPadding(24, 12, 24, 24);
        ScrollView sv = new ScrollView(this); sv.addView(t); root.addView(sv);
        setContentView(root);
    }
}
