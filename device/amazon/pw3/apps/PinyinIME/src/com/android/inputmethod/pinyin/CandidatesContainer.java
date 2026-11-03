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

import com.android.inputmethod.pinyin.PinyinIME.DecodingInfo;

import android.content.Context;
import android.util.AttributeSet;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.View;
import android.view.View.OnTouchListener;
import android.widget.ImageButton;
import android.widget.RelativeLayout;
import android.widget.ViewFlipper;

interface ArrowUpdater {
    void updateArrowStatus();
}


/**
 * Container used to host the two candidate views. When user navigates to
 * another page, the other candidate view takes the place of the current one at
 * once: every frame of an animation would be an update of the e-paper panel.
 * These two candidate views and their parent are hosted by this container.
 * <p>
 * Besides the candidate views, there are two arrow views to show the page
 * forward/backward arrows.
 * </p>
 */
public class CandidatesContainer extends RelativeLayout implements
        OnTouchListener, ArrowUpdater {
    /**
     * Alpha value to show an enabled arrow.
     */
    private static int ARROW_ALPHA_ENABLED = 0xff;

    /**
     * Alpha value to show an disabled arrow. It is not drawn at all: a faint
     * arrow would be grey, which is a slow update of the e-paper panel.
     */
    private static int ARROW_ALPHA_DISABLED = 0;

    /**
     * Listener used to notify IME that user clicks a candidate, or navigate
     * between them.
     */
    private CandidateViewListener mCvListener;

    /**
     * The left arrow button used to show previous page.
     */
    private ImageButton mLeftArrowBtn;

    /**
     * The right arrow button used to show next page.
     */
    private ImageButton mRightArrowBtn;

    /**
     * Decoding result to show.
     */
    private DecodingInfo mDecInfo;

    /**
     * The view used to show candidates. It contains two views. Normally, the
     * candidates are shown one of them. When user navigates to another page,
     * the other one is shown.
     */
    private ViewFlipper mFlipper;

    /**
     * The x offset of the flipper in this container.
     */
    private int xOffsetForFlipper;

    /**
     * Current page number in display.
     */
    private int mCurrentPage = -1;

    public CandidatesContainer(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public void initialize(CandidateViewListener cvListener,
            GestureDetector gestureDetector) {
        mCvListener = cvListener;

        mLeftArrowBtn = (ImageButton) findViewById(R.id.arrow_left_btn);
        mRightArrowBtn = (ImageButton) findViewById(R.id.arrow_right_btn);
        mLeftArrowBtn.setOnTouchListener(this);
        mRightArrowBtn.setOnTouchListener(this);

        mFlipper = (ViewFlipper) findViewById(R.id.candidate_flipper);
        mFlipper.setMeasureAllChildren(true);

        invalidate();
        requestLayout();

        for (int i = 0; i < mFlipper.getChildCount(); i++) {
            CandidateView cv = (CandidateView) mFlipper.getChildAt(i);
            cv.initialize(this, gestureDetector, mCvListener);
        }
    }

    public void showCandidates(PinyinIME.DecodingInfo decInfo,
            boolean enableActiveHighlight) {
        if (null == decInfo) return;
        mDecInfo = decInfo;
        mCurrentPage = 0;

        if (decInfo.isCandidatesListEmpty()) {
            showArrow(mLeftArrowBtn, false);
            showArrow(mRightArrowBtn, false);
        } else {
            showArrow(mLeftArrowBtn, true);
            showArrow(mRightArrowBtn, true);
        }

        for (int i = 0; i < mFlipper.getChildCount(); i++) {
            CandidateView cv = (CandidateView) mFlipper.getChildAt(i);
            cv.setDecodingInfo(mDecInfo);
        }

        CandidateView cv = (CandidateView) mFlipper.getCurrentView();
        cv.showPage(mCurrentPage, 0, enableActiveHighlight);

        updateArrowStatus();
        invalidate();
    }

    public int getCurrentPage() {
        return mCurrentPage;
    }

    public void enableActiveHighlight(boolean enableActiveHighlight) {
        CandidateView cv = (CandidateView) mFlipper.getCurrentView();
        cv.enableActiveHighlight(enableActiveHighlight);
        invalidate();
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        Environment env = Environment.getInstance();
        int measuredWidth = env.getScreenWidth();
        int measuredHeight = getPaddingTop();
        measuredHeight += env.getHeightForCandidates();
        widthMeasureSpec = MeasureSpec.makeMeasureSpec(measuredWidth,
                MeasureSpec.EXACTLY);
        heightMeasureSpec = MeasureSpec.makeMeasureSpec(measuredHeight,
                MeasureSpec.EXACTLY);
        super.onMeasure(widthMeasureSpec, heightMeasureSpec);

        if (null != mLeftArrowBtn) {
            xOffsetForFlipper = mLeftArrowBtn.getMeasuredWidth();
        }
    }

    public boolean activeCurseBackward() {
        if (mFlipper.isFlipping() || null == mDecInfo) {
            return false;
        }

        CandidateView cv = (CandidateView) mFlipper.getCurrentView();

        if (cv.activeCurseBackward()) {
            cv.invalidate();
            return true;
        } else {
            return pageBackward(true, true);
        }
    }

    public boolean activeCurseForward() {
        if (mFlipper.isFlipping() || null == mDecInfo) {
            return false;
        }

        CandidateView cv = (CandidateView) mFlipper.getCurrentView();

        if (cv.activeCursorForward()) {
            cv.invalidate();
            return true;
        } else {
            return pageForward(true, true);
        }
    }

    public boolean pageBackward(boolean animLeftRight,
            boolean enableActiveHighlight) {
        if (null == mDecInfo) return false;

        if (mFlipper.isFlipping() || 0 == mCurrentPage) return false;

        int child = mFlipper.getDisplayedChild();
        int childNext = (child + 1) % 2;
        CandidateView cv = (CandidateView) mFlipper.getChildAt(child);
        CandidateView cvNext = (CandidateView) mFlipper.getChildAt(childNext);

        mCurrentPage--;
        int activeCandInPage = cv.getActiveCandiatePosInPage();
        if (animLeftRight)
            activeCandInPage = mDecInfo.mPageStart.elementAt(mCurrentPage + 1)
                    - mDecInfo.mPageStart.elementAt(mCurrentPage) - 1;

        cvNext.showPage(mCurrentPage, activeCandInPage, enableActiveHighlight);
        showNextView();

        updateArrowStatus();
        return true;
    }

    public boolean pageForward(boolean animLeftRight,
            boolean enableActiveHighlight) {
        if (null == mDecInfo) return false;

        if (mFlipper.isFlipping() || !mDecInfo.preparePage(mCurrentPage + 1)) {
            return false;
        }

        int child = mFlipper.getDisplayedChild();
        int childNext = (child + 1) % 2;
        CandidateView cv = (CandidateView) mFlipper.getChildAt(child);
        int activeCandInPage = cv.getActiveCandiatePosInPage();
        cv.enableActiveHighlight(enableActiveHighlight);

        CandidateView cvNext = (CandidateView) mFlipper.getChildAt(childNext);
        mCurrentPage++;
        if (animLeftRight) activeCandInPage = 0;

        cvNext.showPage(mCurrentPage, activeCandInPage, enableActiveHighlight);
        showNextView();

        updateArrowStatus();
        return true;
    }

    public int getActiveCandiatePos() {
        if (null == mDecInfo) return -1;
        CandidateView cv = (CandidateView) mFlipper.getCurrentView();
        return cv.getActiveCandiatePosGlobal();
    }

    public void updateArrowStatus() {
        if (mCurrentPage < 0) return;
        boolean forwardEnabled = mDecInfo.pageForwardable(mCurrentPage);
        boolean backwardEnabled = mDecInfo.pageBackwardable(mCurrentPage);

        if (backwardEnabled) {
            enableArrow(mLeftArrowBtn, true);
        } else {
            enableArrow(mLeftArrowBtn, false);
        }
        if (forwardEnabled) {
            enableArrow(mRightArrowBtn, true);
        } else {
            enableArrow(mRightArrowBtn, false);
        }
    }

    private void enableArrow(ImageButton arrowBtn, boolean enabled) {
        arrowBtn.setEnabled(enabled);
        if (enabled)
            arrowBtn.setAlpha(ARROW_ALPHA_ENABLED);
        else
            arrowBtn.setAlpha(ARROW_ALPHA_DISABLED);
    }

    private void showArrow(ImageButton arrowBtn, boolean show) {
        if (show)
            arrowBtn.setVisibility(View.VISIBLE);
        else
            arrowBtn.setVisibility(View.INVISIBLE);
    }

    public boolean onTouch(View v, MotionEvent event) {
        if (event.getAction() == MotionEvent.ACTION_DOWN) {
            if (v == mLeftArrowBtn) {
                mCvListener.onToRightGesture();
            } else if (v == mRightArrowBtn) {
                mCvListener.onToLeftGesture();
            }
        } else if (event.getAction() == MotionEvent.ACTION_UP) {
            CandidateView cv = (CandidateView) mFlipper.getCurrentView();
            cv.enableActiveHighlight(true);
        }

        return false;
    }

    // The reason why we handle candiate view's touch events here is because
    // that the view under the focused view may get touch events instead of the
    // focused one.
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        event.offsetLocation(-xOffsetForFlipper, 0);
        CandidateView cv = (CandidateView) mFlipper.getCurrentView();
        cv.onTouchEventReal(event);
        return true;
    }

    // Shows the candidate view which has been prepared for the new page.
    private void showNextView() {
        mFlipper.showNext();
        if (!mLeftArrowBtn.isPressed() && !mRightArrowBtn.isPressed()) {
            CandidateView cv = (CandidateView) mFlipper.getCurrentView();
            cv.enableActiveHighlight(true);
        }
    }
}
