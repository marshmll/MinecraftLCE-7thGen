#pragma once

// Minimal DirectXMath-compatible shim, covering exactly the surface
// Camera.cpp's non-console branch (`#else` after __ORBIS__/__PSVITA__/__PS3__)
// uses: XMMATRIX/XMVECTOR/XMFLOAT4, XMMatrixMultiply, XMMatrixDeterminant,
// XMMatrixInverse, XMStoreFloat4. DirectXMath itself is Windows-only (no
// Linux port), but this is bounded, well-defined linear algebra - not a
// design question - so it's implemented for real (standard cofactor/
// adjugate 4x4 inverse), not stubbed. Layout matches DirectXMath exactly
// (XMMATRIX = 4 row vectors of 4 floats each, 64 bytes total) since
// Camera::prepare() memcpy's a raw FloatBuffer's 16 floats directly into an
// XMMATRIX.

struct XMVECTOR
{
	float v[4];
};

struct XMMATRIX
{
	XMVECTOR r[4];
};

struct XMFLOAT4
{
	float x, y, z, w;
};

// result = a * b, DirectXMath's row-vector convention (row i of result is
// row i of a transformed by b).
inline XMMATRIX XMMatrixMultiply(const XMMATRIX &a, const XMMATRIX &b)
{
	XMMATRIX out;
	for (int i = 0; i < 4; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
				sum += a.r[i].v[k] * b.r[k].v[j];
			out.r[i].v[j] = sum;
		}
	}
	return out;
}

inline float XMMatrixDeterminant_scalar(const XMMATRIX &m)
{
	const float *e = &m.r[0].v[0];
#define M(row, col) e[(row)*4 + (col)]
	float A2323 = M(2,2) * M(3,3) - M(2,3) * M(3,2);
	float A1323 = M(2,1) * M(3,3) - M(2,3) * M(3,1);
	float A1223 = M(2,1) * M(3,2) - M(2,2) * M(3,1);
	float A0323 = M(2,0) * M(3,3) - M(2,3) * M(3,0);
	float A0223 = M(2,0) * M(3,2) - M(2,2) * M(3,0);
	float A0123 = M(2,0) * M(3,1) - M(2,1) * M(3,0);
#undef M
	const float *E = &m.r[0].v[0];
	return  E[0] * ( E[5] * A2323 - E[6] * A1323 + E[7] * A1223 )
		  - E[1] * ( E[4] * A2323 - E[6] * A0323 + E[7] * A0223 )
		  + E[2] * ( E[4] * A1323 - E[5] * A0323 + E[7] * A0123 )
		  - E[3] * ( E[4] * A1223 - E[5] * A0223 + E[6] * A0123 );
}

inline XMVECTOR XMMatrixDeterminant(const XMMATRIX &m)
{
	float det = XMMatrixDeterminant_scalar(m);
	XMVECTOR result = { { det, det, det, det } };
	return result;
}

// Standard cofactor/adjugate 4x4 inverse. detOut is only read (matches
// Camera.cpp's call site, which passes the just-computed determinant vector
// straight through) - recomputed internally rather than trusted blindly, to
// stay correct even if a caller ever passes a stale determinant.
inline XMMATRIX XMMatrixInverse(XMVECTOR *detOut, const XMMATRIX &m)
{
	const float *e = &m.r[0].v[0];
#define M(row, col) e[(row)*4 + (col)]
	float A2323 = M(2,2) * M(3,3) - M(2,3) * M(3,2);
	float A1323 = M(2,1) * M(3,3) - M(2,3) * M(3,1);
	float A1223 = M(2,1) * M(3,2) - M(2,2) * M(3,1);
	float A0323 = M(2,0) * M(3,3) - M(2,3) * M(3,0);
	float A0223 = M(2,0) * M(3,2) - M(2,2) * M(3,0);
	float A0123 = M(2,0) * M(3,1) - M(2,1) * M(3,0);
	float A2313 = M(1,2) * M(3,3) - M(1,3) * M(3,2);
	float A1313 = M(1,1) * M(3,3) - M(1,3) * M(3,1);
	float A1213 = M(1,1) * M(3,2) - M(1,2) * M(3,1);
	float A2312 = M(1,2) * M(2,3) - M(1,3) * M(2,2);
	float A1312 = M(1,1) * M(2,3) - M(1,3) * M(2,1);
	float A1212 = M(1,1) * M(2,2) - M(1,2) * M(2,1);
	float A0313 = M(1,0) * M(3,3) - M(1,3) * M(3,0);
	float A0213 = M(1,0) * M(3,2) - M(1,2) * M(3,0);
	float A0312 = M(1,0) * M(2,3) - M(1,3) * M(2,0);
	float A0212 = M(1,0) * M(2,2) - M(1,2) * M(2,0);
	float A0113 = M(1,0) * M(3,1) - M(1,1) * M(3,0);
	float A0112 = M(1,0) * M(2,1) - M(1,1) * M(2,0);

	float det = M(0,0) * ( M(1,1) * A2323 - M(1,2) * A1323 + M(1,3) * A1223 )
			  - M(0,1) * ( M(1,0) * A2323 - M(1,2) * A0323 + M(1,3) * A0223 )
			  + M(0,2) * ( M(1,0) * A1323 - M(1,1) * A0323 + M(1,3) * A0123 )
			  - M(0,3) * ( M(1,0) * A1223 - M(1,1) * A0223 + M(1,2) * A0123 );

	if (detOut)
	{
		detOut->v[0] = detOut->v[1] = detOut->v[2] = detOut->v[3] = det;
	}

	float invDet = (det != 0.0f) ? (1.0f / det) : 0.0f;

	XMMATRIX out;
	float *o = &out.r[0].v[0];
	o[0]  = invDet *   ( M(1,1) * A2323 - M(1,2) * A1323 + M(1,3) * A1223 );
	o[1]  = invDet * - ( M(0,1) * A2323 - M(0,2) * A1323 + M(0,3) * A1223 );
	o[2]  = invDet *   ( M(0,1) * A2313 - M(0,2) * A1313 + M(0,3) * A1213 );
	o[3]  = invDet * - ( M(0,1) * A2312 - M(0,2) * A1312 + M(0,3) * A1212 );
	o[4]  = invDet * - ( M(1,0) * A2323 - M(1,2) * A0323 + M(1,3) * A0223 );
	o[5]  = invDet *   ( M(0,0) * A2323 - M(0,2) * A0323 + M(0,3) * A0223 );
	o[6]  = invDet * - ( M(0,0) * A2313 - M(0,2) * A0313 + M(0,3) * A0213 );
	o[7]  = invDet *   ( M(0,0) * A2312 - M(0,2) * A0312 + M(0,3) * A0212 );
	o[8]  = invDet *   ( M(1,0) * A1323 - M(1,1) * A0323 + M(1,3) * A0123 );
	o[9]  = invDet * - ( M(0,0) * A1323 - M(0,1) * A0323 + M(0,3) * A0123 );
	o[10] = invDet *   ( M(0,0) * A1313 - M(0,1) * A0313 + M(0,3) * A0113 );
	o[11] = invDet * - ( M(0,0) * A1312 - M(0,1) * A0312 + M(0,3) * A0112 );
	o[12] = invDet * - ( M(1,0) * A1223 - M(1,1) * A0223 + M(1,2) * A0123 );
	o[13] = invDet *   ( M(0,0) * A1223 - M(0,1) * A0223 + M(0,2) * A0123 );
	o[14] = invDet * - ( M(0,0) * A1213 - M(0,1) * A0213 + M(0,2) * A0113 );
	o[15] = invDet *   ( M(0,0) * A1212 - M(0,1) * A0212 + M(0,2) * A0112 );
#undef M
	return out;
}

inline void XMStoreFloat4(XMFLOAT4 *dst, const XMVECTOR &v)
{
	dst->x = v.v[0];
	dst->y = v.v[1];
	dst->z = v.v[2];
	dst->w = v.v[3];
}
