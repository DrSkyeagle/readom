/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.inputmethod.pinyin;

import android.content.Context;
import android.content.res.Configuration;
import android.view.Display;
import android.view.WindowManager;

/**
 * Global environment configurations for showing soft keyboard and candidate
 * view. All original dimension values are defined in float, and the real size
 * is calculated from the float values of and screen size. In this way, this
 * input method can work even when screen size is changed.
 */
public class Environment {
    /**
     * The key height for portrait mode. It is relative to the screen height.
     */
    private static final float KEY_HEIGHT_RATIO_PORTRAIT = 0.075f;

    /**
     * The key height for landscape mode. It is relative to the screen height.
     */
    private static final float KEY_HEIGHT_RATIO_LANDSCAPE = 0.147f;

    /**
     * The height of the candidates area for portrait mode. It is relative to
     * screen height.
     */
    private static final float CANDIDATES_AREA_HEIGHT_RATIO_PORTRAIT = 0.055f;

    /**
     * The height of the candidates area for portrait mode. It is relative to
     * screen height.
     */
    private static final float CANDIDATES_AREA_HEIGHT_RATIO_LANDSCAPE = 0.125f;

    /**
     * The height of the area for the composing string, which is above the
     * candidates area, for portrait mode. It is relative to screen height.
     */
    private static final float COMPOSING_AREA_HEIGHT_RATIO_PORTRAIT = 0.03f;

    /**
     * The height of the area for the composing string for landscape mode. It
     * is relative to screen height.
     */
    private static final float COMPOSING_AREA_HEIGHT_RATIO_LANDSCAPE = 0.045f;

    /**
     * The text size for normal keys. It is relative to the smaller one of
     * screen width and height.
     */
    private static final float NORMAL_KEY_TEXT_SIZE_RATIO = 0.05f;

    /**
     * The text size for function keys. It is relative to the smaller one of
     * screen width and height.
     */
    private static final float FUNCTION_KEY_TEXT_SIZE_RATIO = 0.036f;

    /**
     * The configurations are managed in a singleton.
     */
    private static Environment mInstance;

    private int mScreenWidth;
    private int mScreenHeight;
    private int mKeyHeight;
    private int mCandidatesAreaHeight;
    private int mComposingAreaHeight;
    private int mNormalKeyTextSize;
    private int mFunctionKeyTextSize;
    private Configuration mConfig = new Configuration();
    private boolean mDebug = false;

    private Environment() {
    }

    public static Environment getInstance() {
        if (null == mInstance) {
            mInstance = new Environment();
        }
        return mInstance;
    }

    public void onConfigurationChanged(Configuration newConfig, Context context) {
        if (mConfig.orientation != newConfig.orientation) {
            WindowManager wm = (WindowManager) context
                    .getSystemService(Context.WINDOW_SERVICE);
            Display d = wm.getDefaultDisplay();
            mScreenWidth = d.getWidth();
            mScreenHeight = d.getHeight();

            int scale;
            if (mScreenHeight > mScreenWidth) {
                mKeyHeight = (int) (mScreenHeight * KEY_HEIGHT_RATIO_PORTRAIT);
                mCandidatesAreaHeight = (int) (mScreenHeight * CANDIDATES_AREA_HEIGHT_RATIO_PORTRAIT);
                mComposingAreaHeight = (int) (mScreenHeight * COMPOSING_AREA_HEIGHT_RATIO_PORTRAIT);
                scale = mScreenWidth;
            } else {
                mKeyHeight = (int) (mScreenHeight * KEY_HEIGHT_RATIO_LANDSCAPE);
                mCandidatesAreaHeight = (int) (mScreenHeight * CANDIDATES_AREA_HEIGHT_RATIO_LANDSCAPE);
                mComposingAreaHeight = (int) (mScreenHeight * COMPOSING_AREA_HEIGHT_RATIO_LANDSCAPE);
                scale = mScreenHeight;
            }
            mNormalKeyTextSize = (int) (scale * NORMAL_KEY_TEXT_SIZE_RATIO);
            mFunctionKeyTextSize = (int) (scale * FUNCTION_KEY_TEXT_SIZE_RATIO);
        }

        mConfig.updateFrom(newConfig);
    }

    public Configuration getConfiguration() {
        return mConfig;
    }

    public int getScreenWidth() {
        return mScreenWidth;
    }

    public int getScreenHeight() {
        return mScreenHeight;
    }

    public int getHeightForCandidates() {
        return mCandidatesAreaHeight;
    }

    public int getHeightForComposing() {
        return mComposingAreaHeight;
    }

    public float getKeyXMarginFactor() {
        return 1.0f;
    }

    public float getKeyYMarginFactor() {
        if (Configuration.ORIENTATION_LANDSCAPE == mConfig.orientation) {
            return 0.7f;
        }
        return 1.0f;
    }

    public int getKeyHeight() {
        return mKeyHeight;
    }

    public int getSkbHeight() {
        if (Configuration.ORIENTATION_PORTRAIT == mConfig.orientation) {
            return mKeyHeight * 4;
        } else if (Configuration.ORIENTATION_LANDSCAPE == mConfig.orientation) {
            return mKeyHeight * 4;
        }
        return 0;
    }

    public int getKeyTextSize(boolean isFunctionKey) {
        if (isFunctionKey) {
            return mFunctionKeyTextSize;
        } else {
            return mNormalKeyTextSize;
        }
    }

    public boolean hasHardKeyboard() {
        if (mConfig.keyboard == Configuration.KEYBOARD_NOKEYS
                || mConfig.hardKeyboardHidden == Configuration.HARDKEYBOARDHIDDEN_YES) {
            return false;
        }
        return true;
    }

    public boolean needDebug() {
        return mDebug;
    }
}
