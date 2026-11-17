// Oracle suite of work package P1.5: ICRSplineSegment (Common.dll export,
// common_crspline.cpp). Called from RunUtilSuites in oracle_util.cpp.

#include <stdio.h>
#include <stdlib.h>

#include <string>

#include "oracle_support.h"

#include "common_aisteer.h"

using namespace oracle;

namespace
{
    std::string VectorText(const Vector& V)
    {
        return Format("%08x,%08x,%08x", FloatBits(V.x), FloatBits(V.y), FloatBits(V.z));
    }
}

// Each input line: P0..P3 (12 floats as bits), SampleCount, T, a point (3
// floats), the search start and step.
void RunSplineSuite()
{
    Suite Out("spline.jsonl");
    for (const std::string& Line : InputLines("util/spline.list"))
    {
        unsigned int P[12] = {};
        unsigned int Samples = 0;
        unsigned int Rest[6] = {};
        const int Got = sscanf(Line.c_str(), "%x %x %x %x %x %x %x %x %x %x %x %x %u %x %x %x %x %x %x", &P[0],
                               &P[1], &P[2], &P[3], &P[4], &P[5], &P[6], &P[7], &P[8], &P[9], &P[10], &P[11],
                               &Samples, &Rest[0], &Rest[1], &Rest[2], &Rest[3], &Rest[4], &Rest[5]);
        if (Got != 19)
        {
            fprintf(stderr, "bad spline line %s\n", Line.c_str());
            GFailed = true;
            continue;
        }
        ICRSplineSegment Segment;
        Vector* const Points[4] = {&Segment.P0, &Segment.P1, &Segment.P2, &Segment.P3};
        for (int Index = 0; Index < 4; ++Index)
        {
            Points[Index]->x = FloatFromBits(P[Index * 3]);
            Points[Index]->y = FloatFromBits(P[Index * 3 + 1]);
            Points[Index]->z = FloatFromBits(P[Index * 3 + 2]);
        }
        Segment.SampleCount = Samples;
        const float T = FloatFromBits(Rest[0]);
        Vector Point;
        Point.x = FloatFromBits(Rest[1]);
        Point.y = FloatFromBits(Rest[2]);
        Point.z = FloatFromBits(Rest[3]);
        const float Start = FloatFromBits(Rest[4]);
        const float Step = FloatFromBits(Rest[5]);

        const Vector At = Segment.calculate_spline_vector(T);
        const Vector Tangent = Segment.calculate_spline_tangent(T);
        const float Length = Segment.get_spline_length();
        Segment.calculate_arclength();
        const float Search = Segment.get_t_from_point(Point, Start, Step);
        Out.Line("{\"in\":\"" + Line + "\",\"pt\":\"" + VectorText(At) + "\",\"tan\":\"" + VectorText(Tangent) +
                 "\"" +
                 Format(",\"arc\":\"%08x\",\"len\":\"%08x\",\"search\":\"%08x\"}", FloatBits(Segment.ArcLength),
                        FloatBits(Length), FloatBits(Search)));
    }
}
