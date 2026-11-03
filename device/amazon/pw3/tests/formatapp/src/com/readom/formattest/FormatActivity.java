package com.readom.formattest;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.LinearLayout;

/**
 * Three bands, each a layer the CPU composer has to take without sending the
 * frame to SwiftShader: a buffer in RGB_565, one in RGBA_8888 as the control,
 * and a surface shown four times the size of its buffer.
 *
 * Each band is filled with black and white only, so what the panel ends at does
 * not depend on its calibration: black is the lowest level and white the
 * highest whatever the contrast is. The mid-tones of the conversion are checked
 * exhaustively by the host test; what the device has to answer is whether the
 * layer arrived at all, in the right place, and through the composer rather
 * than through GLES.
 *
 * Two more of what the composer accepts are not here, deliberately. RGB_888
 * cannot be locked for a Canvas (Skia has no three-byte configuration), so no
 * application can produce one this way. A turned layer needs the display
 * rotated, which the panel tests already cover.
 */
public final class FormatActivity extends Activity {
    private static final String TAG = "PW3FormatTest";
    // Bands from the top. The halves of each are what the reader samples.
    static final int[] FORMATS = {PixelFormat.RGB_565, PixelFormat.RGBA_8888, PixelFormat.RGB_565};
    static final boolean[] SCALED = {false, false, true};

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        final LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        for (int i = 0; i < FORMATS.length; ++i) {
            final SurfaceView view = new SurfaceView(this);
            // Above the window rather than behind a hole in it: the bands are
            // then layers of their own whatever the window is, which is what
            // the composer is being asked about, and nothing can paint over
            // them before the reader looks.
            view.setZOrderOnTop(true);
            view.getHolder().setFormat(FORMATS[i]);
            view.getHolder().addCallback(new Band(i, view));
            column.addView(view, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        }
        setContentView(column);
    }

    /** Fills one band: the left half black, the right half white. */
    private static final class Band implements SurfaceHolder.Callback {
        private final int index;
        private final View view;
        Band(int index, View view) { this.index = index; this.view = view; }

        @Override public void surfaceCreated(SurfaceHolder holder) { }

        @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            if (SCALED[index] && (width != view.getWidth() / 4 || height != view.getHeight() / 4)) {
                // The buffer is a quarter of the band in each direction, so the
                // composer has to sample it up. setFixedSize comes back here.
                holder.setFixedSize(Math.max(1, view.getWidth() / 4), Math.max(1, view.getHeight() / 4));
                return;
            }
            final Canvas canvas = holder.lockCanvas();
            if (canvas == null) {
                Log.e(TAG, "band " + index + " format " + FORMATS[index] + " could not be locked");
                return;
            }
            final Paint paint = new Paint();
            paint.setColor(Color.BLACK);
            canvas.drawRect(0, 0, width / 2f, height, paint);
            paint.setColor(Color.WHITE);
            canvas.drawRect(width / 2f, 0, width, height, paint);
            holder.unlockCanvasAndPost(canvas);
            Log.i(TAG, "band " + index + " format " + format + " buffer " + width + "x" + height
                    + " shown " + view.getWidth() + "x" + view.getHeight()
                    + " at " + view.getTop() + ".." + view.getBottom());
        }

        @Override public void surfaceDestroyed(SurfaceHolder holder) { }
    }
}
