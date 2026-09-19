# pragma once

# include <Siv3D.hpp>

# include <algorithm>
# include <array>
# include <cmath>
# include <limits>

// Collision kernel shared by the Native game and its physics tests.
namespace arena
{
    constexpr double Width = 500;
    constexpr double Height = 640;
    constexpr double Radius = 14;
    constexpr double PaddleRadius = 30;

    constexpr double GoalLeft = 170;
    constexpr double GoalRight = 330;
    constexpr double MaxSpeed = 1250;
    constexpr double Friction = 500;
    constexpr double Skin = 0.0001;

    struct Body
    {
        s3d::Vec2 p{ Width / 2, Height / 2 };
        s3d::Vec2 v{};
    };

    struct Segment
    {
        s3d::Vec2 a;
        s3d::Vec2 b;
    };

    inline const std::array<Segment, 6> Walls{{
        {{ 0, 0 }, { 0, Height }},
        {{ Width, 0 }, { Width, Height }},
        {{ 0, 0 }, { GoalLeft, 0 }},
        {{ GoalRight, 0 }, { Width, 0 }},
        {{ 0, Height }, { GoalLeft, Height }},
        {{ GoalRight, Height }, { Width, Height }}
    }};

    inline s3d::Vec2 unit(const s3d::Vec2 value, const s3d::Vec2 fallback = { 0, 1 })
    {
        const double length = value.length();
        return (length > 1e-10) ? value / length : fallback;
    }

    inline s3d::Vec2 limit(const s3d::Vec2 value, const double speed)
    {
        const double length = value.length();
        return (length > speed) ? value * (speed / length) : value;
    }

    inline s3d::Vec2 clampPaddle(const s3d::Vec2 position, const int side)
    {
        return {
            std::clamp(position.x, PaddleRadius, Width - PaddleRadius),
            std::clamp(
                position.y,
                side == 0 ? Height / 2 + PaddleRadius : PaddleRadius,
                side == 0 ? Height - PaddleRadius : Height / 2 - PaddleRadius
            )
        };
    }

    // The inside boundary is flat on the rails and circular around goal mouths.
    inline double lip(const double x)
    {
        if (x <= GoalLeft || x >= GoalRight)
        {
            return Radius;
        }

        const double distance = std::min(x - GoalLeft, GoalRight - x);
        return (distance < Radius)
            ? std::sqrt(std::max(0.0, Radius * Radius - distance * distance))
            : 0;
    }

    inline s3d::Vec2 inside(s3d::Vec2 position)
    {
        position.x = std::clamp(position.x, Radius, Width - Radius);

        const double boundary = lip(position.x);
        position.y = std::clamp(
            position.y,
            boundary,
            Height - boundary
        );

        return position;
    }

    inline bool legal(const s3d::Vec2 position)
    {
        return (position - inside(position)).length() < 0.001;
    }

    inline s3d::Vec2 nearest(const Segment segment, const s3d::Vec2 position)
    {
        const s3d::Vec2 direction = segment.b - segment.a;
        return segment.a + direction * std::clamp(
            (position - segment.a).dot(direction) / direction.dot(direction),
            0.0,
            1.0
        );
    }

    struct Result
    {
        bool paddleHit = false;
        int goal = 0;
        int wallHits = 0;
        s3d::Vec2 contact{};
    };

    inline int goalAt(const s3d::Vec2 position)
    {
        if (position.x < GoalLeft + Radius - 1e-8
            || position.x > GoalRight - Radius + 1e-8)
        {
            return 0;
        }

        if (position.y <= 0)
        {
            return 1;
        }

        if (position.y >= Height)
        {
            return 2;
        }

        return 0;
    }

    // Find the closest legal point outside the expanded paddle circle.
    // The candidate set includes rails, goal lips, and the other paddle.
    inline s3d::Vec2 separate(
        const s3d::Vec2 preferred,
        const s3d::Vec2 paddle,
        const s3d::Vec2* other = nullptr
    )
    {
        constexpr double minimumDistance = Radius + PaddleRadius + Skin;

        s3d::Vec2 best = inside(preferred);
        double bestDistance = std::numeric_limits<double>::infinity();

        auto offer = [&](const s3d::Vec2 candidate)
        {
            if (!legal(candidate)
                || (candidate - paddle).length() < minimumDistance - 0.001
                || (other
                    && (candidate - *other).length() < minimumDistance - 0.001))
            {
                return;
            }

            const double distance =
                (candidate - preferred).dot(candidate - preferred);

            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = candidate;
            }
        };

        offer(inside(preferred));

        for (const s3d::Vec2 center : std::array<s3d::Vec2, 2>{{
            paddle,
            other ? *other : paddle
        }})
        {
            offer(center + unit(preferred - center) * minimumDistance);

            for (const double x : { Radius, Width - Radius })
            {
                const double q =
                    minimumDistance * minimumDistance
                    - (x - center.x) * (x - center.x);

                if (q >= 0)
                {
                    for (const double sign : { -1.0, 1.0 })
                    {
                        offer({
                            x,
                            center.y + sign * std::sqrt(q)
                        });
                    }
                }
            }

            for (const double y : { Radius, Height - Radius, 0.0, Height })
            {
                const double q =
                    minimumDistance * minimumDistance
                    - (y - center.y) * (y - center.y);

                if (q >= 0)
                {
                    for (const double sign : { -1.0, 1.0 })
                    {
                        offer({
                            center.x + sign * std::sqrt(q),
                            y
                        });
                    }
                }
            }

            for (const s3d::Vec2 endpoint : std::array<s3d::Vec2, 4>{{
                { GoalLeft, 0 },
                { GoalRight, 0 },
                { GoalLeft, Height },
                { GoalRight, Height }
            }})
            {
                const s3d::Vec2 delta = endpoint - center;
                const double distance = delta.length();

                if (distance < 1e-9
                    || distance > minimumDistance + Radius
                    || distance < std::abs(minimumDistance - Radius))
                {
                    continue;
                }

                const s3d::Vec2 axis = delta / distance;
                const double along =
                    (minimumDistance * minimumDistance
                        - Radius * Radius
                        + distance * distance)
                    / (2 * distance);
                const double height = std::sqrt(std::max(
                    0.0,
                    minimumDistance * minimumDistance - along * along
                ));
                const s3d::Vec2 tangent{ -axis.y, axis.x };

                offer(center + axis * along + tangent * height);
                offer(center + axis * along - tangent * height);
            }
        }

        if (other)
        {
            const s3d::Vec2 delta = *other - paddle;
            const double distance = delta.length();

            if (distance > 1e-9 && distance <= 2 * minimumDistance)
            {
                const s3d::Vec2 axis = delta / distance;
                const s3d::Vec2 tangent{ -axis.y, axis.x };
                const double height = std::sqrt(std::max(
                    0.0,
                    minimumDistance * minimumDistance - distance * distance * 0.25
                ));

                offer(paddle + delta * 0.5 + tangent * height);
                offer(paddle + delta * 0.5 - tangent * height);
            }
        }

        return best;
    }

    inline void reflect(
        Body& puck,
        const s3d::Vec2 normal,
        const s3d::Vec2 surfaceVelocity = {}
    )
    {
        const double normalSpeed = (puck.v - surfaceVelocity).dot(normal);

        if (normalSpeed < 0)
        {
            puck.v = limit(
                puck.v - normal * (2 * normalSpeed),
                MaxSpeed
            );
        }
    }

    struct Hit
    {
        double time;
        s3d::Vec2 normal{};
        int kind = 0; // 1: rail, 2: local paddle, 3/4: goal, 5: remote paddle
    };

    inline void circleHit(
        Hit& best,
        const s3d::Vec2 offset,
        const s3d::Vec2 relativeVelocity,
        const s3d::Vec2 center,
        const double radius,
        const int kind
    )
    {
        const double a = relativeVelocity.dot(relativeVelocity);
        const double b = offset.dot(relativeVelocity);
        const double c = offset.dot(offset) - radius * radius;

        if (a < 1e-14 || b >= 0)
        {
            return;
        }

        const double discriminant = b * b - a * c;
        if (discriminant < 0)
        {
            return;
        }

        const double time =
            (-b - std::sqrt(discriminant)) / a;

        if (time >= -1e-9 && time <= best.time)
        {
            const double safeTime = std::max(0.0, time);
            best = {
                safeTime,
                unit(offset + relativeVelocity * safeTime),
                kind
            };
        }

        // The center is part of the function's conceptual input. Keeping the
        // argument makes calls at both paddle positions self-documenting.
        (void)center;
    }

    inline void railHit(Hit& best, const Body puck, const Segment segment)
    {
        const s3d::Vec2 tangent = unit(segment.b - segment.a);
        const s3d::Vec2 normalBase{ -tangent.y, tangent.x };
        const double segmentLength = (segment.b - segment.a).length();

        for (const double sign : { -1.0, 1.0 })
        {
            const s3d::Vec2 normal = normalBase * sign;
            const double normalSpeed = puck.v.dot(normal);

            if (normalSpeed >= -1e-10)
            {
                continue;
            }

            const double time =
                (Radius - (puck.p - segment.a).dot(normal)) / normalSpeed;
            const double along = (
                puck.p + puck.v * time - segment.a
            ).dot(tangent);

            if (time >= -1e-9
                && time <= best.time
                && along >= 0
                && along <= segmentLength)
            {
                best = {
                    std::max(0.0, time),
                    normal,
                    1
                };
            }
        }

        // The segment endpoints are rounded, so they need independent CCD.
        circleHit(best, puck.p - segment.a, puck.v, segment.a, Radius, 1);
        circleHit(best, puck.p - segment.b, puck.v, segment.b, Radius, 1);
    }

    // The paddle is kinematic (the player's hand), while the puck is dynamic.
    // CCD considers rails, rounded endpoints, moving circular paddles, and goals.
    inline Result step(
        Body& puck,
        const s3d::Vec2 paddleStart,
        const s3d::Vec2 paddleEnd,
        const bool active,
        const double deltaTime,
        const s3d::Vec2 secondStart = {},
        const s3d::Vec2 secondEnd = {},
        const bool secondActive = false
    )
    {
        Result result;

        if (deltaTime <= 0)
        {
            return result;
        }

        if ((result.goal = goalAt(puck.p)))
        {
            puck.v = {};
            return result;
        }

        const s3d::Vec2 paddleVelocity = (paddleEnd - paddleStart) / deltaTime;
        const s3d::Vec2 secondVelocity = (secondEnd - secondStart) / deltaTime;

        const double travelDistance = std::max({
            puck.v.length() * deltaTime,
            active ? (paddleEnd - paddleStart).length() : 0.0,
            secondActive ? (secondEnd - secondStart).length() : 0.0
        });
        const int substeps = std::max(
            1,
            static_cast<int>(std::ceil(travelDistance / (Radius * 0.25)))
        );
        const double substepTime = deltaTime / substeps;

        for (int substep = 0; substep < substeps; ++substep)
        {
            double elapsed = substep * substepTime;
            double remaining = substepTime;

            const double speed = puck.v.length();
            puck.v = (speed > 0)
                ? puck.v * (std::max(0.0, speed - Friction * substepTime) / speed)
                : s3d::Vec2{};

            for (int iteration = 0; iteration < 16 && remaining > 1e-9; ++iteration)
            {
                const s3d::Vec2 paddle = paddleStart + paddleVelocity * elapsed;
                const s3d::Vec2 secondPaddle = secondStart + secondVelocity * elapsed;

                // Resolve an already-overlapping pose before doing a sweep.
                if (active
                    && (puck.p - paddle).length()
                        < Radius + PaddleRadius - Skin)
                {
                    const s3d::Vec2 corrected = separate(
                        puck.p,
                        paddle,
                        secondActive ? &secondPaddle : nullptr
                    );
                    reflect(puck, unit(corrected - paddle), paddleVelocity);
                    puck.p = corrected;
                    result.paddleHit = true;
                    result.contact = puck.p;
                }

                if (secondActive
                    && (puck.p - secondPaddle).length()
                        < Radius + PaddleRadius - Skin)
                {
                    const s3d::Vec2 corrected = separate(
                        puck.p,
                        secondPaddle,
                        active ? &paddle : nullptr
                    );
                    reflect(puck, unit(corrected - secondPaddle), secondVelocity);
                    puck.p = corrected;
                    result.paddleHit = true;
                    result.contact = puck.p;
                }

                if ((result.goal = goalAt(puck.p)))
                {
                    puck.v = {};
                    return result;
                }

                // Wall constraints have priority during simultaneous contact.
                const s3d::Vec2 constrained = inside(puck.p);
                if ((constrained - puck.p).length() > 1e-8)
                {
                    reflect(puck, unit(constrained - puck.p));
                    puck.p = constrained;
                }

                Hit hit{ remaining + 1e-9 };

                for (const auto wall : Walls)
                {
                    railHit(hit, puck, wall);
                }

                if (active)
                {
                    circleHit(
                        hit,
                        puck.p - paddle,
                        puck.v - paddleVelocity,
                        paddle,
                        Radius + PaddleRadius,
                        2
                    );
                }

                if (secondActive)
                {
                    circleHit(
                        hit,
                        puck.p - secondPaddle,
                        puck.v - secondVelocity,
                        secondPaddle,
                        Radius + PaddleRadius,
                        5
                    );
                }

                // A goal is a crossing of the finite opening, not a wall.
                if (std::abs(puck.v.y) > 1e-10)
                {
                    const double goalY = puck.v.y < 0 ? 0 : Height;
                    const double time = (goalY - puck.p.y) / puck.v.y;
                    const double x = puck.p.x + puck.v.x * time;

                    if (time >= 0
                        && time < hit.time
                        && x >= GoalLeft + Radius
                        && x <= GoalRight - Radius)
                    {
                        hit = {
                            time,
                            {},
                            puck.v.y < 0 ? 3 : 4
                        };
                    }
                }

                if (!hit.kind || hit.time > remaining)
                {
                    puck.p = puck.p + puck.v * remaining;
                    elapsed += remaining;
                    remaining = 0;
                    break;
                }

                puck.p = puck.p + puck.v * hit.time;
                remaining -= hit.time;
                elapsed += hit.time;

                if (hit.kind == 3 || hit.kind == 4)
                {
                    result.goal = hit.kind - 2;
                    puck.v = {};
                    return result;
                }

                if (hit.kind == 2 || hit.kind == 5)
                {
                    reflect(
                        puck,
                        hit.normal,
                        hit.kind == 2 ? paddleVelocity : secondVelocity
                    );
                    result.paddleHit = true;
                    result.contact = puck.p;
                }
                else
                {
                    reflect(puck, hit.normal);
                    ++result.wallHits;
                    result.contact = puck.p;
                }

                puck.p = inside(puck.p + hit.normal * Skin);

                // Resting simultaneous contacts must not loop forever.
                if (iteration == 15)
                {
                    puck.v = {};
                    remaining = 0;
                }
            }
        }

        // Resolve the final paddle pose too, including a contact at the
        // iteration budget boundary.
        if (active
            && (puck.p - paddleEnd).length()
                < Radius + PaddleRadius - Skin)
        {
            const s3d::Vec2 corrected = separate(
                puck.p,
                paddleEnd,
                secondActive ? &secondEnd : nullptr
            );
            reflect(puck, unit(corrected - paddleEnd), paddleVelocity);
            puck.p = corrected;
            result.paddleHit = true;
            result.contact = puck.p;
        }

        if (secondActive
            && (puck.p - secondEnd).length()
                < Radius + PaddleRadius - Skin)
        {
            const s3d::Vec2 corrected = separate(
                puck.p,
                secondEnd,
                active ? &paddleEnd : nullptr
            );
            reflect(puck, unit(corrected - secondEnd), secondVelocity);
            puck.p = corrected;
            result.paddleHit = true;
            result.contact = puck.p;
        }

        puck.p = inside(puck.p);

        if ((result.goal = goalAt(puck.p)))
        {
            puck.v = {};
        }

        return result;
    }
}
