/*
 * Copyright 2022 Bytes & Brains
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <postgres.h> // Datum, etc.
#include <fmgr.h>	  // PG_FUNCTION_ARGS, etc.

#include <h3api.h> // Main H3 include
#include "extension.h"
#include <math.h>
#include "wkb_vect3.h"
#include "wkb.h"

#define SIGN(x) ((x < 0) ? -1 : (x > 0) ? 1 \
										: 0)

PG_FUNCTION_INFO_V1(h3_cell_to_boundary_wkb);

/* Converts CellBoundary coordinates to degrees in place */
static void
			boundary_to_degs(CellBoundary * boundary);

/* Checks if CellBoundary is crossed by antimeridian */
static bool
			boundary_crosses_180(const CellBoundary * boundary);

/* Splits CellBoundary by antimeridian (and 0 meridian around poles) */
static void
			boundary_split_180(const CellBoundary * boundary, CellBoundary * left, CellBoundary * right);

/* Calculates latitude of intersection point between segment and antimeridian or 0 meridian */
static double
			split_180_lat(const LatLng * coord1, const LatLng * coord2);

/* Finds the boundary of the index, converts to EWKB, optionally splits the boundary by 180 meridian */
Datum
h3_cell_to_boundary_wkb(PG_FUNCTION_ARGS)
{
	H3Index		cell = PG_GETARG_H3INDEX(0);
	bool		split = PG_GETARG_BOOL(1);

	H3Error		error;
	bytea	   *wkb;
	CellBoundary boundary;

	error = cellToBoundary(cell, &boundary);
	H3_ERROR(error, "cellToBoundary");

	if (split && boundary_crosses_180(&boundary))
	{
		CellBoundary parts[2];

		boundary_split_180(&boundary, &parts[0], &parts[1]);

		boundary_to_degs(&parts[0]);
		boundary_to_degs(&parts[1]);
		wkb = boundary_array_to_wkb(parts, 2);
	}
	else
	{
		boundary_to_degs(&boundary);
		wkb = boundary_to_wkb(&boundary);
	}

	PG_RETURN_BYTEA_P(wkb);
}

void
boundary_to_degs(CellBoundary * boundary)
{
	LatLng	   *verts = boundary->verts;
	const int	numVerts = boundary->numVerts;

	for (int v = 0; v < numVerts; v++)
	{
		verts[v].lng = radsToDegs(verts[v].lng);
		verts[v].lat = radsToDegs(verts[v].lat);
	}
}

bool
boundary_crosses_180(const CellBoundary * boundary)
{
	const int	numVerts = boundary->numVerts;
	const LatLng *verts = boundary->verts;

	int			prevSign = 0;

	for (int v = 0; v <= numVerts; v++)
	{
		int			cur = v % numVerts;
		double		lon = verts[cur].lng;
		int			sign = SIGN(lon);

		if (prevSign == 0)
		{
			prevSign = sign;
		}
		else if (sign != 0 && prevSign != sign)
		{
			int			prev = (v + numVerts - 1) % numVerts;
			double		prevLon = verts[prev].lng;

			if (fabs(lon) + fabs(prevLon) > M_PI)
				return true;
			else
				prevSign = sign;
		}
	}
	return false;
}

void
boundary_split_180(const CellBoundary * boundary, CellBoundary * part1, CellBoundary * part2)
{
	const int	numVerts = boundary->numVerts;
	const LatLng *verts = boundary->verts;

	CellBoundary *part,
			   *prevPart;
	LatLng		split;
	int			prevSign = 0;
	int			start = 0;		/* current batch start */

	part1->numVerts = 0;
	part2->numVerts = 0;
	for (int v = 0; v <= numVerts; v++)
	{
		int			cur = v % numVerts;
		double		lon = verts[cur].lng;
		int			sign = SIGN(lon);

		if (prevSign != 0 && sign != 0 && sign != prevSign)
		{
			/* Crossing 0 or 180 meridian */

			/*
			 * Assuming boundary is crossed by 180 meridian at least once, so
			 * segment has to be split by either anti or 0 meridian
			 */

			int			prev = (v + numVerts - 1) % numVerts;
			double		prevLon = verts[prev].lng;
			bool		crossesZero = (fabs(lon) + fabs(prevLon) < M_PI);

			prevPart = (prevSign < 0) ? part1 : part2;
			part = (sign < 0) ? part1 : part2;

			/* Add points to prev. part */
			for (int i = start; i < v && i < numVerts; i++)
				prevPart->verts[prevPart->numVerts++] = verts[i];

			/* Calc. split point latitude */
			split.lat = split_180_lat(&verts[cur], &verts[prev]);

			/* Add split point */
			/* prev. part */
			split.lng = crossesZero ? 0 : (prevLon < 0) ? -M_PI
				: M_PI;
			;
			prevPart->verts[prevPart->numVerts++] = split;
			/* current part */
			split.lng = crossesZero ? 0 : -split.lng;
			part->verts[part->numVerts++] = split;

			start = v;			/* start next batch from current point */
		}

		if (sign != 0)
			prevSign = sign;
	}

	/* Add remaining points */
	part = (prevSign < 0) ? part1 : part2;
	for (int i = start; i < numVerts; i++)
		part->verts[part->numVerts++] = verts[i];
}

double
split_180_lat(const LatLng * coord1, const LatLng * coord2)
{
	Vect3		p1,
				p2,
				normal,
				s;
	double		y;

	/* Normal of circle containing points: normal = p1 x p2 */
	vect3_from_lat_lng(coord1, &p1);
	vect3_from_lat_lng(coord2, &p2);
	vect3_cross(&p1, &p2, &normal);

	/* y coordinate of 0/180 meridian circle normal */
	y = (coord1->lng < 0 || coord2->lng > 0) ? -1 : 1;

	/* Circle plane intersection vector: s = (p1 x p2) x {0, y, 0} */
	s.x = -(normal.z * y);
	s.y = 0;
	s.z = normal.x * y;
	vect3_normalize(&s);		/* intersection point coordinates on unit
								 * sphere */

	return asin(s.z);			/* latitude */
}