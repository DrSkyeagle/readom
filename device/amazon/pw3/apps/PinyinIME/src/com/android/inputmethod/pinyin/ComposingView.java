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
import android.content.res.Resources;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Paint.FontMetricsInt;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.View;

/**
 * View used to show composing string (The Pinyin string for the unselected
 * syllables and the Chinese string for the selected syllables.) It is a row of
 * constant size above the candidates.
 */
public class ComposingView extends View {
    /**
     * <p>
     * There are three statuses for the composing view.
     * </p>
     * 
     * <p>
     * {@link #SHOW_PINYIN} is used to show the current Pinyin string without
     * highlighted effect. When user inputs Pinyin characters one by one, the
     * Pinyin string will be shown in this mode.
     * </p>
     * <p>
     * {@link #SHOW_STRING_LOWERCASE} is used to show the Pinyin string in
     * lowercase with highlighted effect. When user presses UP key and there is
     * no fixed Chinese characters, composing view will switch from
     * {@link #SHOW_PINYIN} to this mode, and in this mode, user can press
     * confirm key to input the lower-case string, so that user can input
     * English letter in Chinese mode.
     * </p>
     * <p>
     * {@link #EDIT_PINYIN} is used to edit the Pinyin string (shown with
     * highlighted effect). When current status is {@link #SHOW_PINYIN} and user
     * presses UP key, if there are fixed Characters, the input method will
     * switch to {@link #EDIT_PINYIN} thus user can modify some characters in
     * the middle of the Pinyin string. If the current status is
     * {@link #SHOW_STRING_LOWERCASE} and user presses LEFT and RIGHT key, it
     * will also switch to {@link #EDIT_PINYIN}.
     * </p>
     * <p>
     * Whenever user presses down key, the status switches to
     * {@link #SHOW_PINYIN}.
     * </p>
     * <p>
     * When composing view's status is {@link #SHOW_PINYIN}, the IME's status is
     * {@link PinyinIME.ImeState#STATE_INPUT}, otherwise, the IME's status
     * should be {@link PinyinIME.ImeState#STATE_COMPOSING}.
     * </p>
     */
    public enum ComposingStatus {
        SHOW_PINYIN, SHOW_STRING_LOWERCASE, EDIT_PINYIN,
    }

    private static final int LEFT_RIGHT_MARGIN = 5;

    /**
     * The width in pixels of the cursor for editing mode.
     */
    private static final int CURSOR_WIDTH = 2;

    /**
     * Used to draw composing string. For the highlighted string, the color is
     * changed.
     */
    private Paint mPaint;

    /**
     * Used to draw the highlight effect and the cursor for editing mode. It is
     * not anti-aliased, so that they have no grey edge, which would be a slow
     * update of the e-paper panel.
     */
    private Paint mFillPaint;

    /**
     * Used to estimate dimensions to show the string .
     */
    private FontMetricsInt mFmi;

    private int mStrColor;
    private int mStrColorHl;

    private ComposingStatus mComposingStatus;

    PinyinIME.DecodingInfo mDecInfo;

    public ComposingView(Context context, AttributeSet attrs) {
        super(context, attrs);

        Resources r = context.getResources();
        mStrColor = r.getColor(R.color.black);
        mStrColorHl = r.getColor(R.color.white);

        mPaint = new Paint();
        mPaint.setColor(mStrColor);
        mPaint.setAntiAlias(true);

        mFillPaint = new Paint();
        mFillPaint.setColor(mStrColor);

        // The largest text which fits into the height of the row.
        int height = Environment.getInstance().getHeightForComposing();
        int textSize = 1;
        mPaint.setTextSize(textSize);
        mFmi = mPaint.getFontMetricsInt();
        while (mFmi.bottom - mFmi.top <= height) {
            textSize++;
            mPaint.setTextSize(textSize);
            mFmi = mPaint.getFontMetricsInt();
        }
        textSize--;
        mPaint.setTextSize(textSize);
        mFmi = mPaint.getFontMetricsInt();
    }

    public void reset() {
        mComposingStatus = ComposingStatus.SHOW_PINYIN;
    }

    /**
     * Set the composing string to show. If the IME status is
     * {@link PinyinIME.ImeState#STATE_INPUT}, the composing view's status will
     * be set to {@link ComposingStatus#SHOW_PINYIN}, otherwise the composing
     * view will set its status to {@link ComposingStatus#SHOW_STRING_LOWERCASE}
     * or {@link ComposingStatus#EDIT_PINYIN} automatically.
     */
    public void setDecodingInfo(PinyinIME.DecodingInfo decInfo,
            PinyinIME.ImeState imeStatus) {
        mDecInfo = decInfo;

        if (PinyinIME.ImeState.STATE_INPUT == imeStatus) {
            mComposingStatus = ComposingStatus.SHOW_PINYIN;
            mDecInfo.moveCursorToEdge(false);
        } else {
            if (decInfo.getFixedLen() != 0
                    || ComposingStatus.EDIT_PINYIN == mComposingStatus) {
                mComposingStatus = ComposingStatus.EDIT_PINYIN;
            } else {
                mComposingStatus = ComposingStatus.SHOW_STRING_LOWERCASE;
            }
            mDecInfo.moveCursor(0);
        }

        invalidate();
    }

    public boolean moveCursor(int keyCode) {
        if (keyCode != KeyEvent.KEYCODE_DPAD_LEFT
                && keyCode != KeyEvent.KEYCODE_DPAD_RIGHT) return false;

        if (ComposingStatus.EDIT_PINYIN == mComposingStatus) {
            int offset = 0;
            if (keyCode == KeyEvent.KEYCODE_DPAD_LEFT)
                offset = -1;
            else if (keyCode == KeyEvent.KEYCODE_DPAD_RIGHT) offset = 1;
            mDecInfo.moveCursor(offset);
        } else if (ComposingStatus.SHOW_STRING_LOWERCASE == mComposingStatus) {
            if (keyCode == KeyEvent.KEYCODE_DPAD_LEFT
                    || keyCode == KeyEvent.KEYCODE_DPAD_RIGHT) {
                mComposingStatus = ComposingStatus.EDIT_PINYIN;
            }

        }
        invalidate();
        return true;
    }

    public ComposingStatus getComposingStatus() {
        return mComposingStatus;
    }

    // The size does not depend on the composing string, so that the views
    // below, and the application above, stay where they are while user types.
    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        Environment env = Environment.getInstance();
        setMeasuredDimension(env.getScreenWidth(), env.getHeightForComposing());
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (ComposingStatus.EDIT_PINYIN == mComposingStatus
                || ComposingStatus.SHOW_PINYIN == mComposingStatus) {
            drawForPinyin(canvas);
            return;
        }

        float x, y;
        x = mPaddingLeft + LEFT_RIGHT_MARGIN;
        y = getTextBaseline();

        String splStr = mDecInfo.getOrigianlSplStr().toString();
        float width = mPaint.measureText(splStr, 0, splStr.length());
        canvas.drawRect(mPaddingLeft, mPaddingTop, (int) (x + width + 0.5f)
                + LEFT_RIGHT_MARGIN, getHeight() - mPaddingBottom, mFillPaint);

        mPaint.setColor(mStrColorHl);
        canvas.drawText(splStr, 0, splStr.length(), x, y, mPaint);
    }

    // The text is centered in the height of the view.
    private int getTextBaseline() {
        int height = getHeight() - mPaddingTop - mPaddingBottom;
        return mPaddingTop + (height - (mFmi.bottom - mFmi.top)) / 2 - mFmi.top;
    }

    private void drawCursor(Canvas canvas, float x) {
        canvas.drawRect((int) x, mPaddingTop, (int) x + CURSOR_WIDTH,
                getHeight() - mPaddingBottom, mFillPaint);
    }

    private void drawForPinyin(Canvas canvas) {
        float x, y;
        x = mPaddingLeft + LEFT_RIGHT_MARGIN;
        y = getTextBaseline();

        mPaint.setColor(mStrColor);

        int cursorPos = mDecInfo.getCursorPosInCmpsDisplay();
        int cmpsPos = cursorPos;
        String cmpsStr = mDecInfo.getComposingStrForDisplay();
        int activeCmpsLen = mDecInfo.getActiveCmpsDisplayLen();
        if (cursorPos > activeCmpsLen) cmpsPos = activeCmpsLen;
        canvas.drawText(cmpsStr, 0, cmpsPos, x, y, mPaint);
        x += mPaint.measureText(cmpsStr, 0, cmpsPos);
        if (cursorPos <= activeCmpsLen) {
            if (ComposingStatus.EDIT_PINYIN == mComposingStatus) {
                drawCursor(canvas, x);
            }
            canvas.drawText(cmpsStr, cmpsPos, activeCmpsLen, x, y, mPaint);
        }

        x += mPaint.measureText(cmpsStr, cmpsPos, activeCmpsLen);

        if (cmpsStr.length() > activeCmpsLen) {
            int oriPos = activeCmpsLen;
            if (cursorPos > activeCmpsLen) {
                if (cursorPos > cmpsStr.length()) cursorPos = cmpsStr.length();
                canvas.drawText(cmpsStr, oriPos, cursorPos, x, y, mPaint);
                x += mPaint.measureText(cmpsStr, oriPos, cursorPos);

                if (ComposingStatus.EDIT_PINYIN == mComposingStatus) {
                    drawCursor(canvas, x);
                }

                oriPos = cursorPos;
            }
            canvas.drawText(cmpsStr, oriPos, cmpsStr.length(), x, y, mPaint);
        }
    }
}
