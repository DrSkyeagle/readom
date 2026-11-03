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

import com.android.inputmethod.pinyin.SoftKeyboard.KeyRow;

import java.util.List;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Paint.FontMetricsInt;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.View;

/**
 * Class used to show a soft keyboard.
 * 
 * A soft keyboard view should not handle touch event itself, because we do bias
 * correction, need a global strategy to map an event into a proper view to
 * achieve better user experience.
 */
public class SoftKeyboardView extends View {
    /**
     * The definition of the soft keyboard for the current this soft keyboard
     * view.
     */
    private SoftKeyboard mSoftKeyboard;

    /** The last key pressed. */
    private SoftKey mSoftKeyDown;

    /** Used to indicate whether the user is holding on a key. */
    private boolean mKeyPressed = false;

    /**
     * Text size for normal key.
     */
    private int mNormalKeyTextSize;

    /**
     * Text size for function key.
     */
    private int mFunctionKeyTextSize;

    /**
     * Long press timer used to response long-press.
     */
    private SkbContainer.LongPressTimer mLongPressTimer;

    private Paint mPaint;
    private FontMetricsInt mFmi = new FontMetricsInt();

    public SoftKeyboardView(Context context, AttributeSet attrs) {
        super(context, attrs);

        mPaint = new Paint();
        mPaint.setAntiAlias(true);
    }

    public boolean setSoftKeyboard(SoftKeyboard softSkb) {
        if (null == softSkb) {
            return false;
        }
        mSoftKeyboard = softSkb;
        Drawable bg = softSkb.getSkbBackground();
        if (null != bg) setBackgroundDrawable(bg);
        return true;
    }

    public SoftKeyboard getSoftKeyboard() {
        return mSoftKeyboard;
    }

    public void resizeKeyboard(int skbWidth, int skbHeight) {
        mSoftKeyboard.setSkbCoreSize(skbWidth, skbHeight);
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int measuredWidth = 0;
        int measuredHeight = 0;
        if (null != mSoftKeyboard) {
            measuredWidth = mSoftKeyboard.getSkbCoreWidth();
            measuredHeight = mSoftKeyboard.getSkbCoreHeight();
            measuredWidth += mPaddingLeft + mPaddingRight;
            measuredHeight += mPaddingTop + mPaddingBottom;
        }
        setMeasuredDimension(measuredWidth, measuredHeight);
    }

    // Redraws one key. The time the panel takes for an update grows with the
    // area, so a key press must not make the whole keyboard dirty.
    private void invalidateKey(SoftKey softKey) {
        invalidate(mPaddingLeft + softKey.mLeft, mPaddingTop + softKey.mTop,
                mPaddingLeft + softKey.mRight, mPaddingTop + softKey.mBottom);
    }

    public void resetKeyPress() {
        if (!mKeyPressed) return;
        mKeyPressed = false;
        invalidateKey(mSoftKeyDown);
    }

    // If movePress is true, means that this function is called because user
    // moves his finger to this button. If movePress is false, means that this
    // function is called when user just presses this key.
    public SoftKey onKeyPress(int x, int y,
            SkbContainer.LongPressTimer longPressTimer, boolean movePress) {
        mKeyPressed = false;
        boolean moveWithinPreviousKey = false;
        // The keys are laid out inside the padding of the view.
        x -= mPaddingLeft;
        y -= mPaddingTop;
        if (movePress) {
            SoftKey newKey = mSoftKeyboard.mapToKey(x, y);
            if (newKey == mSoftKeyDown) moveWithinPreviousKey = true;
            mSoftKeyDown = newKey;
        } else {
            mSoftKeyDown = mSoftKeyboard.mapToKey(x, y);
        }
        if (moveWithinPreviousKey || null == mSoftKeyDown) return mSoftKeyDown;
        mKeyPressed = true;

        mLongPressTimer = longPressTimer;

        if (!movePress) {
            if (mSoftKeyDown.getPopupResId() > 0 || mSoftKeyDown.repeatable()) {
                mLongPressTimer.startTimer();
            }
        } else {
            mLongPressTimer.removeTimer();
        }

        invalidateKey(mSoftKeyDown);
        return mSoftKeyDown;
    }

    public SoftKey onKeyRelease(int x, int y) {
        mKeyPressed = false;
        if (null == mSoftKeyDown) return null;

        mLongPressTimer.removeTimer();

        invalidateKey(mSoftKeyDown);

        if (mSoftKeyDown.moveWithinKey(x - mPaddingLeft, y - mPaddingTop)) {
            return mSoftKeyDown;
        }
        return null;
    }

    public SoftKey onKeyMove(int x, int y) {
        if (null == mSoftKeyDown) return null;

        if (mSoftKeyDown.moveWithinKey(x - mPaddingLeft, y - mPaddingTop)) {
            return mSoftKeyDown;
        }

        // The current key needs to be updated. When user moves between keys,
        // repeated response is disabled.
        SoftKey previousKey = mSoftKeyDown;
        SoftKey newKey = onKeyPress(x, y, mLongPressTimer, true);
        if (newKey != previousKey) invalidateKey(previousKey);
        return newKey;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (null == mSoftKeyboard) return;

        canvas.translate(mPaddingLeft, mPaddingTop);

        Environment env = Environment.getInstance();
        mNormalKeyTextSize = env.getKeyTextSize(false);
        mFunctionKeyTextSize = env.getKeyTextSize(true);
        // Draw the last soft keyboard
        int rowNum = mSoftKeyboard.getRowNum();
        int keyXMargin = mSoftKeyboard.getKeyXMargin();
        int keyYMargin = mSoftKeyboard.getKeyYMargin();
        for (int row = 0; row < rowNum; row++) {
            KeyRow keyRow = mSoftKeyboard.getKeyRowForDisplay(row);
            if (null == keyRow) continue;
            List<SoftKey> softKeys = keyRow.mSoftKeys;
            int keyNum = softKeys.size();
            for (int i = 0; i < keyNum; i++) {
                SoftKey softKey = softKeys.get(i);
                if (SoftKeyType.KEYTYPE_ID_NORMAL_KEY == softKey.mKeyType.mKeyTypeId) {
                    mPaint.setTextSize(mNormalKeyTextSize);
                } else {
                    mPaint.setTextSize(mFunctionKeyTextSize);
                }
                drawSoftKey(canvas, softKey, keyXMargin, keyYMargin);
            }
        }
    }

    private void drawSoftKey(Canvas canvas, SoftKey softKey, int keyXMargin,
            int keyYMargin) {
        // A pressed key differs in its background only, and there only in
        // pixels which are pure black or pure white in both states: the panel
        // shows such a change in its fastest update. The label keeps its
        // color, because the edges of its glyphs are grey.
        Drawable bg;
        if (mKeyPressed && softKey == mSoftKeyDown) {
            bg = softKey.getKeyHlBg();
        } else {
            bg = softKey.getKeyBg();
        }

        if (null != bg) {
            bg.setBounds(softKey.mLeft + keyXMargin, softKey.mTop + keyYMargin,
                    softKey.mRight - keyXMargin, softKey.mBottom - keyYMargin);
            bg.draw(canvas);
        }

        String keyLabel = softKey.getKeyLabel();
        Drawable keyIcon = softKey.getKeyIcon();
        if (null != keyIcon) {
            Drawable icon = keyIcon;
            int marginLeft = (softKey.width() - icon.getIntrinsicWidth()) / 2;
            int marginRight = softKey.width() - icon.getIntrinsicWidth()
                    - marginLeft;
            int marginTop = (softKey.height() - icon.getIntrinsicHeight()) / 2;
            int marginBottom = softKey.height() - icon.getIntrinsicHeight()
                    - marginTop;
            icon.setBounds(softKey.mLeft + marginLeft,
                    softKey.mTop + marginTop, softKey.mRight - marginRight,
                    softKey.mBottom - marginBottom);
            icon.setColorFilter(softKey.getIconFilter());
            icon.draw(canvas);
        } else if (null != keyLabel) {
            mPaint.setColor(softKey.getColor());
            mPaint.getFontMetricsInt(mFmi);
            float x = softKey.mLeft
                    + (softKey.width() - mPaint.measureText(keyLabel)) / 2.0f;
            float y = softKey.mTop
                    + (softKey.height() - mFmi.ascent - mFmi.descent) / 2.0f;
            canvas.drawText(keyLabel, x, y, mPaint);
        }
    }
}
