/*
 * FTGL - OpenGL font library
 *
 * Copyright (c) 2001-2004 Henry Maddocks <ftgl@opengl.geek.nz>
 * Copyright (c) 2008 Sam Hocevar <sam@zoy.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"
#include <iostream>

#include <math.h>
#include <cstring>

#include "FTGL/ftgl.h"
#include "FTGL/ErrorCheck.h"

#include "FTInternals.h"
#include "FTTextureGlyphImpl.h"

using namespace std;

//
//  FTGLTextureGlyph
//

FTTextureGlyph::FTTextureGlyph(FT_GlyphSlot glyph, int id, int xOffset,
                               int yOffset, int width, int height)
    : FTGlyph(
          new FTTextureGlyphImpl(glyph, id, xOffset, yOffset, width, height))
{
}

FTTextureGlyph::~FTTextureGlyph() {}

const FTPoint& FTTextureGlyph::Render(const FTPoint& pen, int renderMode)
{
    FTTextureGlyphImpl* myimpl = dynamic_cast<FTTextureGlyphImpl*>(impl);
    return myimpl->RenderImpl(pen, renderMode);
}

//
//  FTGLTextureGlyphImpl
//

GLint FTTextureGlyphImpl::activeTextureID = 0;

FTTextureGlyphImpl::FTTextureGlyphImpl(FT_GlyphSlot glyph, int id, int xOffset,
                                       int yOffset, int width, int height)
    : FTGlyphImpl(glyph)
    , destWidth(0)
    , destHeight(0)
    , glTextureID(id)
    , xOffsetCached(xOffset)
    , yOffsetCached(yOffset)
    , bitmapData(0)
    , uploaded(false)
{
    /* FIXME: need to propagate the render mode all the way down to
     * here in order to get FT_RENDER_MODE_MONO aliased fonts.
     */

    err = FT_Render_Glyph(glyph, FT_RENDER_MODE_NORMAL);
    if (err || glyph->format != ft_glyph_format_bitmap)
    {
        return;
    }

    FT_Bitmap bitmap = glyph->bitmap;

    destWidth = bitmap.width;
    destHeight = bitmap.rows;

    if (destWidth && destHeight)
    {
        // =====================================================================
        // BAND-AID — workaround for host-application architectural debt.
        // =====================================================================
        // FTGL itself is correct: uploading a glyph bitmap into a GL atlas
        // from a constructor is standard OpenGL. This workaround exists
        // because OpenRV's Metal/GL bridge has a re-entrancy race that
        // silently drops in-flight glTexSubImage2D commands.
        //
        // The race, in detail:
        //   OpenRV runs OpenGL on Apple Silicon via a Metal/IOSurface
        //   bridge. Any call that drains the Metal command queue
        //   synchronously -- glFinish (explicit or inside the driver) and
        //   some internal sync points -- pumps the Qt event loop inline. A
        //   pending MTKView drawable-size-change event then runs its
        //   IOSurface-FBO-recreation handler re-entrantly, unbinds the
        //   atlas texture, and discards the in-flight glTexSubImage2D
        //   before it commits to GPU memory. The result is a zero-filled
        //   region in the font atlas -- visually, a blank glyph.
        //
        // Evidence that FTGL is not the broken component:
        //   The bug disappears after a "clear session" -- not because the
        //   FTGL path is broken on first load, but because by the time
        //   glyphs are re-created the MTKView has finished its resize
        //   dance and no re-entrant events are pending. The upload code is
        //   fine; the surrounding GL/Qt state is what varies.
        //
        // Smoking-gun diagnostic (2026-04-21 investigation):
        //   [FTGL POST-UP-BIND] xOff=419 boundTex=4 atlasW=16384 ... OK
        //   [FTGL POST-UP-BIND] xOff=437 boundTex=0 atlasW=0    ... unbound
        //   [RVMetalView] Drawable size will change: 2316 x 1826
        //   [FTGL READBACK]    xOff=437  sum=0  nonzero=0/510  ... data lost
        //
        // Workaround applied here:
        //   Copy the FT_Bitmap buffer in the constructor (the slot buffer
        //   is only valid until the next FT_Load_Glyph/FT_Load_Char, so it
        //   must be copied). Defer glTexSubImage2D to the first RenderImpl
        //   call, which always executes inside a stable draw pass with no
        //   pending resize events.
        //
        // The correct architectural fix is in the host application, not
        // here. See src/lib/app/RvCommon/RVMetalView.mm
        // (drawableSizeWillChange:) for the pointer-comment tracking this
        // debt, and TECH_DEBT.md ("Metal/GL Qt event-loop re-entrancy
        // race") for the catalogued entry.
        // =====================================================================
        size_t nbytes = (size_t)destWidth * (size_t)destHeight;
        bitmapData = new unsigned char[nbytes];
        memcpy(bitmapData, bitmap.buffer, nbytes);
    }

    //      0
    //      +----+
    //      |    |
    //      |    |
    //      |    |
    //      +----+
    //           1

    uv[0].X(static_cast<float>(xOffset) / static_cast<float>(width));
    uv[0].Y(static_cast<float>(yOffset) / static_cast<float>(height));
    uv[1].X(static_cast<float>(xOffset + destWidth)
            / static_cast<float>(width));
    uv[1].Y(static_cast<float>(yOffset + destHeight)
            / static_cast<float>(height));

    corner = FTPoint(glyph->bitmap_left, glyph->bitmap_top);
}

FTTextureGlyphImpl::~FTTextureGlyphImpl()
{
    delete[] bitmapData;
}

const FTPoint& FTTextureGlyphImpl::RenderImpl(const FTPoint& pen,
                                              int renderMode)
{
    float dx, dy;

    if (activeTextureID != glTextureID)
    {
        glBindTexture(GL_TEXTURE_2D, (GLuint)glTextureID);
        activeTextureID = glTextureID;
    }

    // First-time upload of the glyph bitmap. BAND-AID: deferred from the
    // constructor to avoid a Metal/GL Qt event-loop re-entrancy race. See
    // the BAND-AID banner on the constructor above (and TECH_DEBT.md)
    // for the full explanation.
    if (!uploaded && bitmapData && destWidth > 0 && destHeight > 0)
    {
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

        // A GL_PIXEL_UNPACK_BUFFER bound by some other code path (e.g.
        // RV's image upload path) would cause glTexSubImage2D to treat
        // bitmapData as a byte offset into that PBO. Unbind around the
        // upload, then restore.
        GLint savedUnpackBuffer = 0;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &savedUnpackBuffer);
        if (savedUnpackBuffer) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        glTexSubImage2D(GL_TEXTURE_2D, 0, xOffsetCached, yOffsetCached,
                        destWidth, destHeight, GL_ALPHA, GL_UNSIGNED_BYTE,
                        bitmapData);

        if (savedUnpackBuffer) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, savedUnpackBuffer);

        FTGL_GLDEBUG

        glPopClientAttrib();

        uploaded = true;
        delete[] bitmapData;
        bitmapData = 0;
    }

    dx = floor(pen.Xf() + corner.Xf());
    dy = floor(pen.Yf() + corner.Yf());

    glBegin(GL_QUADS);
    glTexCoord2f(uv[0].Xf(), uv[0].Yf());
    glVertex2f(dx, dy);

    glTexCoord2f(uv[0].Xf(), uv[1].Yf());
    glVertex2f(dx, dy - destHeight);

    glTexCoord2f(uv[1].Xf(), uv[1].Yf());
    glVertex2f(dx + destWidth, dy - destHeight);

    glTexCoord2f(uv[1].Xf(), uv[0].Yf());
    glVertex2f(dx + destWidth, dy);
    glEnd();

    return advance;
}
