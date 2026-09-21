# include <Siv3D.hpp>
# include "ArenaPhysics.hpp"
# include "WebRTCP2PClient.hpp"

# include <functional>

#if SIV3D_PLATFORM(WEB)
# include <emscripten.h>
EM_JS(void, ToggleSiv3DFullscreen, (),
{
    if (typeof globalThis.siv3dToggleFullscreen === "function")
    {
        globalThis.siv3dToggleFullscreen();
    }
});
EM_JS(int, IsSiv3DFullscreen, (),
{
    return (document.fullscreenElement || document.webkitFullscreenElement) ? 1 : 0;
});
#else
void ToggleSiv3DFullscreen() {}
bool IsSiv3DFullscreen()
{
    return Window::GetState().fullscreen;
}
#endif

namespace
{
    using Client = s3d::WebRTCP2PClient;

    // -------------------------------------------------------------------------
    // Match and network model
    // -------------------------------------------------------------------------

    constexpr Client::MessageID StatePacketId = 200;
    constexpr Client::MessageID MovePacketId = 201;
    constexpr Client::MessageID ImpactPacketId = 202;
    constexpr Client::MessageID GoalPacketId = 203;
    constexpr Client::MessageID AssignmentPacketId = 204;
    constexpr Client::MessageID RematchRequestPacketId = 205;
    constexpr Client::MessageID RematchStatusPacketId = 206;
    constexpr int WinningScore = 7;

    constexpr double CourtScale = 1.16;
    const RectF Court{
        (720.0 - arena::Width * CourtScale) / 2.0,
        200,
        arena::Width * CourtScale,
        arena::Height * CourtScale
    };

    const ColorF Ink{ 0.965, 0.980, 1.0 };
    const ColorF Panel{ 1.0, 1.0, 1.0 };
    const ColorF White{ 1.0, 1.0, 1.0 };
    const ColorF Text{ 0.12, 0.16, 0.25 };
    const ColorF Muted{ 0.42, 0.49, 0.60 };
    const ColorF Outline{ 0.24, 0.29, 0.38 };
    const ColorF Red{ 0.92, 0.22, 0.32 };
    const ColorF Blue{ 0.15, 0.40, 0.88 };
    const ColorF Yellow{ 1.0, 0.78, 0.16 };

    Vec2 ToViewPosition(Vec2 position, const int side)
    {
        if (side == 1)
        {
            position = Vec2{ arena::Width, arena::Height } - position;
        }

        return Court.pos + position * CourtScale;
    }

    Vec2 ToWorldPosition(Vec2 position, const int side)
    {
        position = (position - Court.pos) / CourtScale;

        if (side == 1)
        {
            position = Vec2{ arena::Width, arena::Height } - position;
        }

        return position;
    }

    enum class Phase : uint8
    {
        Waiting,
        Countdown,
        Playing,
        Goal,
        Finished,
    };

    enum class Screen : uint8
    {
        Title,
        Game,
        Lobby,
    };

    struct MatchState
    {
        uint32 match = 0;
        uint32 round = 0;

        Vec2 hostPaddle{ 250, 562 };
        Vec2 guestPaddle{ 250, 78 };
        Vec2 puck{ 250, 320 };
        Vec2 puckVelocity{ 0, 0 };

        int32 hostScore = 0;
        int32 guestScore = 0;
        int32 scorer = -1;

        Phase phase = Phase::Waiting;
        double timer = 0;

        template <class Archive>
        void SIV3D_SERIALIZE(Archive& archive)
        {
            archive(
                match,
                round,
                hostPaddle,
                guestPaddle,
                puck,
                puckVelocity,
                hostScore,
                guestScore,
                scorer,
                phase,
                timer
            );
        }

        Vec2& PaddlePosition(const int side)
        {
            return (side == 0) ? hostPaddle : guestPaddle;
        }

        const Vec2& PaddlePosition(const int side) const
        {
            return (side == 0) ? hostPaddle : guestPaddle;
        }

        int Score(const int side) const
        {
            return (side == 0) ? hostScore : guestScore;
        }
    };

    struct MovePacket
    {
        uint8 side = 0;
        Vec2 position;

        template <class Archive>
        void SIV3D_SERIALIZE(Archive& archive)
        {
            archive(side, position);
        }
    };

    struct ImpactPacket
    {
        uint32 match = 0;
        uint32 round = 0;
        uint8 side = 0;
        Vec2 puck;
        Vec2 velocity;
        Vec2 paddle;

        template <class Archive>
        void SIV3D_SERIALIZE(Archive& archive)
        {
            archive(match, round, side, puck, velocity, paddle);
        }
    };

    struct AssignmentPacket
    {
        bool spectator = false;

        template <class Archive>
        void SIV3D_SERIALIZE(Archive& archive)
        {
            archive(spectator);
        }
    };

    struct RematchRequestPacket
    {
        bool ready = false;

        template <class Archive>
        void SIV3D_SERIALIZE(Archive& archive)
        {
            archive(ready);
        }
    };

    struct RematchStatusPacket
    {
        uint8 readyCount = 0;

        template <class Archive>
        void SIV3D_SERIALIZE(Archive& archive)
        {
            archive(readyCount);
        }
    };

    struct GoalPacket
    {
        uint32 match = 0;
        uint32 round = 0;
        int32 scorer = 0;
        Vec2 position;

        template <class Archive>
        void SIV3D_SERIALIZE(Archive& archive)
        {
            archive(match, round, scorer, position);
        }
    };

    // -------------------------------------------------------------------------
    // Effects
    // -------------------------------------------------------------------------

    struct Spark
    {
        Vec2 position;
        Vec2 velocity;
        double life = 0;
        double totalLife = 0;
        ColorF color;
    };

    struct Effects
    {
        Array<Spark> sparks;
        Trail trail{ 0.42, EaseOutQuad, EaseInQuad };
        int trailSide = -1;

        double ring = 0;
        double flash = 0;
        double soundCooldown = 0;
        double wallSoundCooldown = 0;
        Vec2 ringPosition;
        ColorF ringColor = Red;

        int rally = 0;
        int bestRally = 0;
        bool muted = false;

        Audio hit;
        Audio wall;
        Audio goal;

        static Audio MakeTone(
            const double frequency,
            const double duration,
            const double slide
        )
        {
            Wave wave{
                static_cast<size_t>(44100 * duration),
                Arg::sampleRate = 44100
            };

            for (size_t i = 0; i < wave.size(); ++i)
            {
                const double time = static_cast<double>(i) / 44100;
                const double envelope =
                    std::exp(-time * 8 / duration) * Min(1.0, time * 1000);
                const float sample = static_cast<float>(
                    std::sin(Math::TwoPi * (frequency * time + slide * time * time))
                    * envelope
                    * 0.26
                );

                wave[i].left = sample;
                wave[i].right = sample;
            }

            return Audio{ wave };
        }

        Effects()
            : hit(MakeTone(410, 0.16, -500))
            , wall(MakeTone(220, 0.12, -220))
            , goal(MakeTone(660, 0.7, 300))
        {
        }

        void Burst(const Vec2 position, const ColorF color, const int count = 14)
        {
            for (int i = 0; i < count; ++i)
            {
                const double angle = Random(0.0, Math::TwoPi);
                const double life = Random(0.2, 0.6);

                sparks.push_back({
                    position,
                    Vec2{ Cos(angle), Sin(angle) } * Random(45.0, 190.0),
                    life,
                    life,
                    color
                });
            }
        }

        void PaddleContact(const Vec2 position, const ColorF paddleColor)
        {
            ++rally;
            bestRally = Max(bestRally, rally);
            ring = 0.35;
            ringPosition = position;
            ringColor = paddleColor;
            Burst(position, paddleColor, 10);

            if (!muted && soundCooldown <= 0)
            {
                hit.playOneShot(0.5);
                soundCooldown = 0.04;
            }
        }

        void WallContact(const Vec2 position)
        {
            Burst(position, Yellow, 5);

            if (!muted && wallSoundCooldown <= 0)
            {
                wall.playOneShot(0.60);
                wallSoundCooldown = 0.03;
            }
        }

        void Scored(const Vec2 position)
        {
            flash = 0.6;
            trail.clear();
            Burst(position, Red, 50);
            rally = 0;

            if (!muted)
            {
                goal.playOneShot(0.5);
            }
        }

        void Update(
            const double deltaTime,
            const Vec2 puck,
            const int side,
            const bool playing
        )
        {
            if (trailSide != side)
            {
                trail.clear();
                trailSide = side;
            }

            ring = Max(0.0, ring - deltaTime);
            flash = Max(0.0, flash - deltaTime);
            soundCooldown = Max(0.0, soundCooldown - deltaTime);
            wallSoundCooldown = Max(0.0, wallSoundCooldown - deltaTime);
            trail.update(deltaTime);

            for (auto& spark : sparks)
            {
                spark.life -= deltaTime;
                spark.position += spark.velocity * deltaTime;
                spark.velocity *= std::exp(-3 * deltaTime);
            }

            sparks.remove_if([](const Spark& spark) {
                return spark.life <= 0;
            });

            const Vec2 viewPuck = ToViewPosition(puck, side);
            if (playing
                && (trail.num_points() == 0
                    || trail.back().pos.distanceFrom(viewPuck) > 3))
            {
                trail.add(viewPuck, ColorF{ 1.0, 0.76, 0.16, 0.26 }, 20.0 * CourtScale);
            }
        }

        void Draw(const int side) const
        {
            trail.draw();

            for (const auto& spark : sparks)
            {
                Circle{
                    ToViewPosition(spark.position, side),
                    1 + 2 * spark.life / spark.totalLife
                }.draw(ColorF{ spark.color, spark.life / spark.totalLife });
            }

            if (ring > 0)
            {
                Circle{
                    ToViewPosition(ringPosition, side),
                    25 + (1 - ring / 0.35) * 45
                }.drawFrame(2, ColorF{ ringColor, ring / 0.35 });
            }
        }
    };

    // -------------------------------------------------------------------------
    // Rendering helpers
    // -------------------------------------------------------------------------

    bool DrawButton(
        const RectF rect,
        const StringView label,
        const bool enabled = true,
        const bool primary = false
    )
    {
        const bool hovered = enabled && rect.mouseOver();

        rect.rounded(8).draw(
            enabled
                ? (primary
                    ? ColorF{ Red, hovered ? 1.0 : 0.90 }
                    : (hovered
                        ? ColorF{ 0.90, 0.95, 1.0 }
                        : ColorF{ 0.985, 0.99, 1.0 }))
                : ColorF{ 0.90, 0.93, 0.97 }
        );

        if (!primary)
        {
            rect.rounded(8).drawFrame(
                1,
                ColorF{ Outline, enabled ? 0.55 : 0.25 }
            );
        }

        FontAsset(U"Body")(label).drawAt(
            rect.center(),
            enabled ? Text : Muted
        );

        if (hovered)
        {
            Cursor::RequestStyle(CursorStyle::Hand);
        }

        return enabled && rect.leftClicked();
    }

    bool DrawRematchButton(
        const RectF rect,
        const StringView label,
        const bool enabled,
        const bool selfReady,
        const bool opponentReady,
        const ColorF selfColor,
        const ColorF opponentColor
    )
    {
        const bool hovered = enabled && rect.mouseOver();
        const ColorF fill = selfReady
            ? ColorF{ selfColor, hovered ? 1.0 : 0.90 }
            : (hovered ? ColorF{ 0.90, 0.95, 1.0 } : Panel);
        const ColorF border = opponentReady ? opponentColor : Outline;
        const double borderAlpha = opponentReady ? 0.95 : 0.42;

        rect.rounded(8).draw(fill);
        rect.rounded(8).drawFrame(
            opponentReady ? 2.5 : 1.0,
            ColorF{ border, enabled ? borderAlpha : 0.20 }
        );
        FontAsset(U"Body")(label).drawAt(
            rect.center(),
            selfReady ? White : Text
        );

        if (hovered)
        {
            Cursor::RequestStyle(CursorStyle::Hand);
        }

        return enabled && rect.leftClicked();
    }

    bool DrawIconButton(
        const RectF rect,
        const StringView icon,
        const bool enabled = true
    )
    {
        const bool hovered = enabled && rect.mouseOver();

        rect.rounded(8).draw(
            enabled
                ? (hovered
                    ? ColorF{ 0.90, 0.95, 1.0 }
                    : ColorF{ 0.985, 0.99, 1.0 })
                : ColorF{ 0.90, 0.93, 0.97 }
        );
        rect.rounded(8).drawFrame(
            1,
            ColorF{ Outline, enabled ? 0.55 : 0.25 }
        );

        FontAsset(U"Icon")(icon).drawAt(
            rect.center(),
            enabled ? (hovered ? Blue : Text) : Muted
        );

        if (hovered)
        {
            Cursor::RequestStyle(CursorStyle::Hand);
        }

        return enabled && rect.leftClicked();
    }

    bool DrawButtonWithIcon(
        const RectF rect,
        const StringView icon,
        const StringView label,
        const bool enabled = true,
        const bool primary = false
    )
    {
        const bool hovered = enabled && rect.mouseOver();

        rect.rounded(8).draw(
            enabled
                ? (primary
                    ? ColorF{ Red, hovered ? 1.0 : 0.90 }
                    : (hovered
                        ? ColorF{ 0.90, 0.95, 1.0 }
                        : ColorF{ 0.985, 0.99, 1.0 }))
                : ColorF{ 0.90, 0.93, 0.97 }
        );

        if (!primary)
        {
            rect.rounded(8).drawFrame(
                1,
                ColorF{ Outline, enabled ? 0.55 : 0.25 }
            );
        }

        const ColorF textColor = enabled ? Text : Muted;
        const double iconWidth = FontAsset(U"Icon")(icon).region().w;
        const double labelWidth = FontAsset(U"Body")(label).region().w;
        const double gap = 8;
        const double contentWidth = iconWidth + gap + labelWidth;
        const double contentLeft = rect.center().x - contentWidth / 2;

        FontAsset(U"Icon")(icon).drawAt(
            Vec2{ contentLeft + iconWidth / 2, rect.center().y },
            textColor
        );
        FontAsset(U"Body")(label).drawAt(
            Vec2{ contentLeft + iconWidth + gap + labelWidth / 2, rect.center().y },
            textColor
        );

        if (hovered)
        {
            Cursor::RequestStyle(CursorStyle::Hand);
        }

        return enabled && rect.leftClicked();
    }

    bool DrawRefreshButton(const Vec2 center, const bool enabled = true)
    {
        const RectF hitBox{ center - Vec2{ 25, 25 }, Vec2{ 50, 50 } };
        const bool hovered = enabled && hitBox.mouseOver();
        const ColorF color = enabled ? (hovered ? Blue : Muted) : ColorF{ Muted, 0.35 };

        hitBox.rounded(12).draw(ColorF{ Panel, hovered ? 1.0 : 0.82 });
        hitBox.rounded(12).drawFrame(1, ColorF{ Outline, 0.35 });
        FontAsset(U"Icon")(U"\uf021").drawAt(center, color);

        if (hovered)
        {
            Cursor::RequestStyle(CursorStyle::Hand);
        }

        return enabled && hitBox.leftClicked();
    }

    void DrawPanel(const RectF rect)
    {
        rect.movedBy(0, 5).rounded(14).draw(ColorF{ Outline, 0.10 });
        rect.rounded(14).draw(Panel);
        rect.rounded(14).drawFrame(1.5, ColorF{ Outline, 0.28 });
    }

    void DrawPaddle(const Vec2 position, const ColorF color)
    {
        Circle{ position + Vec2{ 0, 7 * CourtScale }, 32 * CourtScale }
            .draw(ColorF{ 0, 0.35 });
        Circle{ position, 39 * CourtScale }.draw(ColorF{ color, 0.06 });
        Circle{ position, 34 * CourtScale }.draw(ColorF{ color, 0.10 });
        Circle{ position, 30 * CourtScale }.draw(ColorF{ 0.15, 0.20, 0.30 });
        Circle{ position, 28 * CourtScale }.draw(color);
        Circle{ position, 23 * CourtScale }.draw(
            ColorF{ color.r * 0.55, color.g * 0.55, color.b * 0.55 }
        );
        Circle{ position, 23 * CourtScale }
            .drawFrame(1 * CourtScale, ColorF{ White, 0.4 });
        Circle{ position + Vec2{ 0, -2 * CourtScale }, 14 * CourtScale }
            .draw(color);
        Circle{ position + Vec2{ -3 * CourtScale, -6 * CourtScale }, 7 * CourtScale }
            .draw(ColorF{ White, 0.16 });
    }

    void DrawPaddleCaption(const Vec2 position, const StringView text, const ColorF color)
    {
        const Vec2 captionPosition = position + Vec2{ 0, -48 * CourtScale };
        FontAsset(U"Small")(text).drawAt(captionPosition + Vec2{ 0, 2 }, ColorF{ 0, 0.15, 0.25, 0.25 });
        FontAsset(U"Small")(text).drawAt(captionPosition, color);
    }

    void DrawCourt(
        const MatchState& game,
        const int side,
        const Vec2 hostPaddle,
        const Vec2 guestPaddle,
        Effects& effects
    )
    {
        RectF{ Court.pos - Vec2{ 19, 19 }, Court.size + Vec2{ 38, 38 } }
            .rounded(22)
            .draw(ColorF{ Outline, 0.14 });
        RectF{ Court.pos - Vec2{ 13, 13 }, Court.size + Vec2{ 26, 26 } }
            .rounded(18)
            .draw(ColorF{ 0.98, 0.98, 0.99 });
        RectF{ Court.pos - Vec2{ 13, 13 }, Court.size + Vec2{ 26, 26 } }
            .rounded(18)
            .drawFrame(2, ColorF{ Outline, 0.42 });
        Court.draw(ColorF{ 0.88, 0.95, 1.0 });

        for (int row = 0; row < 28; ++row)
        {
            for (int column = 0; column < 22; ++column)
            {
                Circle{
                    Court.pos + Vec2{ 19.0 + column * 22, 20.0 + row * 22 } * CourtScale,
                    0.8 * CourtScale
                }.draw(ColorF{ 0.26, 0.33, 0.46, 0.12 });
            }
        }

        Line{
            Court.x,
            Court.center().y,
            Court.x + Court.w,
            Court.center().y
        }.draw(2.0 * CourtScale, ColorF{ 0.26, 0.33, 0.46, 0.34 });
        Circle{ Court.center(), 72 * CourtScale }
            .drawFrame(2.0 * CourtScale, ColorF{ 0.26, 0.33, 0.46, 0.34 });
        Circle{ Court.center(), 7 * CourtScale }
            .draw(ColorF{ 0.26, 0.33, 0.46, 0.24 });

        // Render exactly the finite segments used by the physics engine.
        for (const auto& wall : arena::Walls)
        {
            const Vec2 start = ToViewPosition((wall.a), side);
            const Vec2 end = ToViewPosition((wall.b), side);

            Line{ start, end }.draw(12 * CourtScale, ColorF{ Outline, 0.10 });
            Line{ start, end }.draw(5 * CourtScale, ColorF{ Outline, 0.82 });
            Circle{ start, 2.5 * CourtScale }.draw(ColorF{ Outline, 0.82 });
            Circle{ end, 2.5 * CourtScale }.draw(ColorF{ Outline, 0.82 });
        }

        for (int end = 0; end < 2; ++end)
        {
            const double y = end ? Court.y + Court.h : Court.y;
            // The screen is flipped for the Client viewpoint, so resolve the
            // goal color in world space before drawing it.
            const int worldEnd = (side == 1) ? (1 - end) : end;
            const ColorF color = worldEnd ? Red : Blue;

            for (int glow = 4; glow > 0; --glow)
            {
                RectF{
                    Court.x + arena::GoalLeft * CourtScale,
                    y - 3.0 * glow * CourtScale,
                    (arena::GoalRight - arena::GoalLeft) * CourtScale,
                    6.0 * glow * CourtScale
                }.draw(ColorF{ color, 0.06 });
            }

            Line{
                Court.x + arena::GoalLeft * CourtScale,
                y,
                Court.x + arena::GoalRight * CourtScale,
                y
            }.draw(3 * CourtScale, color);
        }

        effects.Draw(side);
        // Host is always Red and Client is always Blue, regardless of the
        // viewpoint used to render the court.
        DrawPaddle(ToViewPosition(hostPaddle, side), Red);
        DrawPaddle(ToViewPosition(guestPaddle, side), Blue);

        const Vec2 puck = ToViewPosition(game.puck, side);
        Circle{ puck + Vec2{ 0, 5 * CourtScale }, arena::Radius * CourtScale }
            .draw(ColorF{ 0.15, 0.25, 0.40, 0.20 });
        Circle{ puck, 28 * CourtScale }.draw(ColorF{ 1.0, 0.74, 0.20, 0.025 });
        Circle{ puck, 22 * CourtScale }.draw(ColorF{ 1.0, 0.74, 0.20, 0.055 });
        Circle{ puck, arena::Radius * CourtScale }.draw(ColorF{ 1.0, 0.74, 0.20 });
        Circle{ puck, 9 * CourtScale }.draw(ColorF{ 0.15, 0.20, 0.30 });
        Circle{ puck, 5 * CourtScale }.draw(ColorF{ 1.0, 0.92, 0.52 });

        if (effects.flash > 0)
        {
            Court.draw(ColorF{ Red, effects.flash * 0.12 });
        }
    }

    void PrepareServe(MatchState& game, const int serveToward)
    {
        ++game.round;
        game.puck = { 250, 320 };
        game.puckVelocity = {
            Random(-65.0, 65.0),
            serveToward == 0 ? 420.0 : -420.0
        };
        game.phase = Phase::Countdown;
        game.timer = 3.0;
    }

    void ResetMatch(MatchState& game)
    {
        game = MatchState{};
        game.match = Random<uint32>(1, 0x7fffffffu);
        PrepareServe(game, Random(0, 1));
    }
}

class AirHockeyGame
{
private:
    MatchState game;
    Effects effects;
    Client client{ []
    {
        Client::ClientOptions options;
        options.signalingURL =
            U"wss://webrtc-p2p-signaling.webrtc-p2p-demo.workers.dev/signal";
        options.applicationId = U"air-hockey-arena-v2";
        return options;
    }() };

    bool practice = true;
    bool online = false;
    bool mouseControl = false;
    double mousePaddleOffsetY = 0;
    bool awaitingGoal = false;
    bool memberReady = false;
    Screen screen = Screen::Title;
    bool matchmaking = false;
    bool matchmakingRequestStarted = false;
    bool matchmakingRetryPending = false;
    bool roomListLoading = false;
    Vec2 remotePaddle{ 250, 78 };
    Vec2 spectatorHostPaddle;
    Vec2 spectatorGuestPaddle;
    Client::PeerID opponentPeerID;
    bool spectator = false;
    bool rematchRequested = false;
    bool remoteRematchRequested = false;
    uint8 rematchReadyCount = 0;
    Vec2 lastSentPosition{ -999, -999 };
    double sendClock = 0;
    double idleClock = 0;
    double phaseAge = 0;
    int lastCountdown = -1;
    double roomListRefreshClock = 5.0;
    String centerNotice;
    double centerNoticeTimer = 0;
    int renderSide = 0;
    int renderViewSide = 0;
    int renderCpuSide = 1;
    bool renderLinked = false;
    bool renderCpuActive = true;
    bool renderActive = false;
    bool renderAuthority = false;

    void ShowCenterNotice(const String& text)
    {
        centerNotice = text;
        centerNoticeTimer = 1.6;
    }

    void ClearRematchState()
    {
        rematchRequested = false;
        remoteRematchRequested = false;
        rematchReadyCount = 0;
    }

    void SendStateToOthers()
    {
        if (online && client.getRole() == Client::Role::Host)
        {
            (void)client.send(StatePacketId, Client::SendTarget::Others(), game);
        }
    }

    void SendStateToPeer(const Client::PeerID& peerID)
    {
        if (online && client.getRole() == Client::Role::Host)
        {
            Array<Client::PeerID> recipients{ peerID };
            (void)client.send(
                StatePacketId,
                Client::SendTarget::Peers(recipients),
                game
            );
        }
    }

    void SendAssignment(const Client::PeerID& peerID, const bool isSpectator)
    {
        if (client.getRole() == Client::Role::Host)
        {
            Array<Client::PeerID> recipients{ peerID };
            (void)client.send(
                AssignmentPacketId,
                Client::SendTarget::Peers(recipients),
                AssignmentPacket{ isSpectator }
            );
        }
    }

    void SendRematchStatus()
    {
        if (online
            && client.getRole() == Client::Role::Host
            && !opponentPeerID.isEmpty())
        {
            Array<Client::PeerID> recipients{ opponentPeerID };
            (void)client.send(
                RematchStatusPacketId,
                Client::SendTarget::Peers(recipients),
                RematchStatusPacket{ rematchReadyCount }
            );
        }
    }

    void StartNewMatch()
    {
        ClearRematchState();
        ResetMatch(game);
        effects.trail.clear();
        effects.rally = 0;
        effects.bestRally = 0;
        awaitingGoal = false;
        phaseAge = 0;
        lastCountdown = -1;
        remotePaddle = game.guestPaddle;
        SendStateToOthers();
    }

    void RefreshRoomList()
    {
        if (!roomListLoading)
        {
            roomListLoading = client.refreshRoomList();
            roomListRefreshClock = 0;
        }
    }

    void StartMatchmakingRequest()
    {
        if (!matchmaking || matchmakingRequestStarted)
        {
            return;
        }

        Client::JoinRandomRoomOptions options;
        options.requiredRoomProperties[1] = U"Neon Rally / v2 / matchmaking";
        options.expectedParticipantCount = 1;
        options.expectedMaxParticipants = 8;

        Client::RoomCreationInfo creationInfo;
        creationInfo.listed = true;
        creationInfo.isOpen = true;
        creationInfo.maxParticipants = 8;
        creationInfo.properties[1] = U"Neon Rally / v2 / matchmaking";

        matchmakingRequestStarted = client.joinRandomOrCreateRoom(
            options,
            Client::GenerateRandomRoomId(),
            creationInfo
        );

        if (matchmakingRequestStarted)
        {
            matchmakingRetryPending = false;
        }
        else
        {
            matchmakingRetryPending = true;
            roomListRefreshClock = 0;
        }
    }

    void BeginMatchmaking()
    {
        if (client.getState() != Client::ClientState::Disconnected)
        {
            return;
        }

        screen = Screen::Game;
        practice = true;
        matchmaking = true;
        matchmakingRequestStarted = false;
        matchmakingRetryPending = false;
        online = false;
        StartMatchmakingRequest();
    }

    void LeaveOnlineRoom()
    {
        if (client.getState() != Client::ClientState::Disconnected)
        {
            client.leaveRoom();
        }

        online = false;
        practice = true;
        matchmaking = false;
        matchmakingRequestStarted = false;
        matchmakingRetryPending = false;
        memberReady = false;
        opponentPeerID.clear();
        spectator = false;
        ClearRematchState();
        ResetMatch(game);
    }

    void AwardGoal(const int scorer)
    {
        if (game.phase != Phase::Playing)
        {
            return;
        }

        if (scorer == 0)
        {
            ++game.hostScore;
        }
        else
        {
            ++game.guestScore;
        }

        game.scorer = scorer;
        game.puckVelocity = { 0, 0 };
        game.timer = 1.3;
        phaseAge = 0;
        game.phase = (game.Score(scorer) >= WinningScore)
            ? Phase::Finished
            : Phase::Goal;

        effects.Scored(game.puck);
        awaitingGoal = false;
        SendStateToOthers();
    }

    void UpdateMenuInput(
        const bool linked,
        const bool active,
        const bool authority
    )
    {
        if (screen == Screen::Lobby)
        {
            if (RectF{ 36, 25, 56, 56 }.leftClicked())
            {
                screen = Screen::Game;
                return;
            }

            if (client.getState() == Client::ClientState::Disconnected
                && RectF{ 258, 25, 350, 56 }.leftClicked())
            {
                Client::RoomCreationInfo creationInfo;
                creationInfo.listed = true;
                creationInfo.isOpen = true;
                creationInfo.maxParticipants = 8;
                creationInfo.properties[1] = U"Neon Rally / v2 / manual";

                if (client.createRoom(
                        Client::GenerateRandomRoomId(),
                        creationInfo
                    ))
                {
                    screen = Screen::Game;
                    practice = true;
                    matchmaking = true;
                    matchmakingRequestStarted = true;
                    matchmakingRetryPending = false;
                    online = false;
                }
            }

            if (client.getState() == Client::ClientState::Disconnected
                && RectF{ 625, 106, 50, 50 }.leftClicked())
            {
                RefreshRoomList();
            }

            const auto& rooms = client.getRoomList();
            const size_t visibleRoomCount = Min<size_t>(rooms.size(), 10);
            for (size_t index = 0; index < visibleRoomCount; ++index)
            {
                const auto& roomInfo = rooms[index];
                const double y = 180 + static_cast<double>(index) * 78;
                const bool joinable =
                    roomInfo.isOpen
                    && roomInfo.participantCount < roomInfo.maxParticipants;
                const bool isCurrentRoom = roomInfo.id == client.getRoomID();

                if (joinable
                    && !isCurrentRoom
                    && RectF{ 36, y, 648, 64 }.leftClicked())
                {
                    if (client.joinRoom(roomInfo.id))
                    {
                        screen = Screen::Game;
                        practice = true;
                        matchmaking = false;
                        matchmakingRequestStarted = false;
                    }
                    break;
                }
            }

            return;
        }

        if (screen != Screen::Game)
        {
            return;
        }

        const bool disconnected =
            client.getState() == Client::ClientState::Disconnected;
        const bool waitingForOpponent =
            matchmaking
            || (online
                && client.getRole() == Client::Role::Host
                && opponentPeerID.isEmpty());

        if (RectF{ 36, 24, 56, 56 }.leftClicked())
        {
#if SIV3D_PLATFORM(WEB)
            ToggleSiv3DFullscreen();
#else
            Window::SetFullscreen(!Window::GetState().fullscreen);
#endif
        }

        if (RectF{ 104, 24, 472, 56 }.leftClicked()
            && (waitingForOpponent || disconnected || online))
        {
            if (waitingForOpponent || online)
            {
                LeaveOnlineRoom();
            }
            else
            {
                BeginMatchmaking();
            }
        }

        if (RectF{ 586, 24, 98, 56 }.leftClicked())
        {
            screen = Screen::Lobby;
            RefreshRoomList();
        }

        const bool rematchAvailable = active
            && (online ? linked : authority);
        const RectF rematchRect{
            Court.x + Court.w - 100,
            Court.y - 74,
            100,
            46
        };

        if (rematchRect.leftClicked()
            && rematchAvailable)
        {
            if (!online)
            {
                StartNewMatch();
            }
            else
            {
                rematchRequested = !rematchRequested;

                if (client.getRole() == Client::Role::Host)
                {
                    rematchReadyCount = static_cast<uint8>(
                        (rematchRequested ? 1 : 0)
                        + (remoteRematchRequested ? 1 : 0)
                    );
                    SendRematchStatus();

                    if (rematchReadyCount == 2)
                    {
                        StartNewMatch();
                    }
                }
                else
                {
                    rematchReadyCount = rematchRequested
                        ? Max<uint8>(rematchReadyCount, 1)
                        : 0;
                    (void)client.send(
                        RematchRequestPacketId,
                        Client::SendTarget::Others(),
                        RematchRequestPacket{ rematchRequested }
                    );
                }
            }
        }
    }

public:
    AirHockeyGame()
{
    client.onJoinedRoom = [&](const Client::RoomInfo&)
    {
        online = true;
        opponentPeerID = (client.getRole() == Client::Role::Client)
            ? client.getHostPeerID()
            : Client::PeerID{};
        practice = true;
        effects.trail.clear();
        effects.rally = 0;
        effects.bestRally = 0;
        memberReady = false;
        ClearRematchState();
        lastSentPosition = { -999, -999 };
    };

    client.onMemberLeft = [&](const Client::PeerID& lostPeerID, const Client::MemberLeaveReason)
    {
        if (!online)
        {
            return;
        }

        if (lostPeerID == opponentPeerID)
        {
            if (client.getRole() != Client::Role::Host)
            {
                ShowCenterNotice(U"相手が退出しました");
                // Keep the room. The signaling server promotes a remaining
                // member to Host after the old Host leaves. Until that role
                // change arrives, continue the local CPU match instead of
                // putting the game into a blocked waiting state.
                opponentPeerID.clear();
                ResetMatch(game);
                ClearRematchState();
                practice = true;
                matchmaking = false;
                matchmakingRequestStarted = false;
                matchmakingRetryPending = false;
                awaitingGoal = false;
                memberReady = false;
                spectator = false;
                return;
            }

            opponentPeerID.clear();

            for (const auto& peerID : client.getMemberIDs())
            {
                if (peerID != lostPeerID)
                {
                    opponentPeerID = peerID;
                    SendAssignment(peerID, false);
                    break;
                }
            }

            ShowCenterNotice(opponentPeerID.isEmpty()
                ? U"相手が退出しました"
                : U"相手が切り換りました");

            ResetMatch(game);
            ClearRematchState();
            practice = opponentPeerID.isEmpty();
            matchmaking = opponentPeerID.isEmpty();
            matchmakingRequestStarted = opponentPeerID.isEmpty();
            matchmakingRetryPending = false;
            awaitingGoal = false;
            memberReady = !opponentPeerID.isEmpty();
            if (!opponentPeerID.isEmpty())
            {
                SendStateToPeer(opponentPeerID);
            }
            return;
        }

        if (client.getRole() == Client::Role::Host
            && opponentPeerID.isEmpty())
        {
            // Keep the local CPU match alive while the Host reconnects.
            ResetMatch(game);
            ClearRematchState();
            practice = true;
            spectator = false;
            awaitingGoal = false;
            memberReady = false;
        }
    };

    client.onReconnectingRoom = [&](const Client::ReconnectReason)
    {
        if (online)
        {
            ResetMatch(game);
            ClearRematchState();
            practice = true;
            spectator = false;
            awaitingGoal = false;
            memberReady = false;
        }
    };

    client.onMemberJoined = [&](const Client::PeerID& connectedPeerID)
    {
        if (client.getRole() == Client::Role::Host)
        {
            const bool assignAsSpectator =
                !opponentPeerID.isEmpty()
                && opponentPeerID != connectedPeerID;

            if (!assignAsSpectator)
            {
                opponentPeerID = connectedPeerID;
            }

            SendAssignment(connectedPeerID, assignAsSpectator);

            if (assignAsSpectator)
            {
                SendStateToPeer(connectedPeerID);
            }
            else if (game.match == 0 || matchmaking)
            {
                StartNewMatch();
            }
            else if (game.phase == Phase::Waiting)
            {
                PrepareServe(game, 0);
                SendStateToOthers();
            }
            else
            {
                SendStateToOthers();
            }

            if (!assignAsSpectator)
            {
                ShowCenterNotice(U"対戦相手とマッチングしました");
            }
        }
        else
        {
            if (opponentPeerID.isEmpty()
                && connectedPeerID == client.getHostPeerID())
            {
                opponentPeerID = connectedPeerID;
            }
        }
        practice = false;
        matchmaking = false;
        matchmakingRequestStarted = false;
        matchmakingRetryPending = false;
        memberReady = true;
    };

    client.onMemberReconnected = client.onMemberJoined;

    client.onRoomListUpdated = [&]
    {
        roomListLoading = false;
        roomListRefreshClock = 0;

        if (matchmaking && !matchmakingRequestStarted)
        {
            StartMatchmakingRequest();
        }
    };

    client.onRoomError = [&](const Client::RoomError& error)
    {
        Logger << U"[Arena] " << error.message;

        if (!matchmaking)
        {
            return;
        }

        matchmakingRequestStarted = false;
        matchmakingRetryPending = true;
        online = false;
        practice = true;
        roomListLoading = false;
        roomListRefreshClock = 0;

        if (error.code == Client::RoomErrorCode::RoomFull
            || error.code == Client::RoomErrorCode::RoomNotFound
            || error.code == Client::RoomErrorCode::RoomAlreadyExists)
        {
        }
    };

    client.onMessage = [&](const Client::PeerID& sender,
                         const Client::MessageID messageID,
                         Deserializer<MemoryViewReader>& reader)
    {
        const int side = (client.getRole() == Client::Role::Client) ? 1 : 0;

        if (!online)
        {
            return;
        }

        if (!spectator && side == 1 && sender != client.getHostPeerID())
        {
            return;
        }

        if (messageID == AssignmentPacketId && side == 1)
        {
            AssignmentPacket assignment;
            reader(assignment);
            const bool wasSpectator = spectator;
            spectator = assignment.spectator;
            if (spectator)
            {
                spectatorHostPaddle = game.hostPaddle;
                spectatorGuestPaddle = game.guestPaddle;
            }
            else if (wasSpectator)
            {
                ShowCenterNotice(U"対戦に移りました");
            }
            else if (!spectator)
            {
                ShowCenterNotice(U"対戦相手とマッチングしました");
            }
            practice = false;
            memberReady = true;
            return;
        }
        else if (messageID == RematchStatusPacketId
            && side == 1
            && !spectator
            && sender == client.getHostPeerID())
        {
            RematchStatusPacket status;
            reader(status);
            rematchReadyCount = Min<uint8>(status.readyCount, 2);
            return;
        }
        else if (messageID == RematchRequestPacketId
            && side == 0
            && sender == opponentPeerID)
        {
            RematchRequestPacket request;
            reader(request);
            remoteRematchRequested = request.ready;
            rematchReadyCount =
                static_cast<uint8>(rematchRequested ? 1 : 0)
                + static_cast<uint8>(remoteRematchRequested ? 1 : 0);
            SendRematchStatus();

            if (rematchReadyCount == 2)
            {
                StartNewMatch();
            }
            return;
        }
        else if (messageID == StatePacketId && side == 1)
        {
            MatchState next;
            reader(next);

            const bool freshMatch = next.match != game.match;
            if (!freshMatch && next.round < game.round)
            {
                return;
            }

            const bool scored =
                (next.hostScore != game.hostScore)
                || (next.guestScore != game.guestScore);
            const Vec2 localPaddle = game.guestPaddle;

            game = next;

            if (!spectator && !freshMatch)
            {
                game.guestPaddle = localPaddle;
            }

            if (spectator || freshMatch)
            {
                spectatorHostPaddle = game.hostPaddle;
                spectatorGuestPaddle = game.guestPaddle;
            }

            if (scored)
            {
                effects.Scored(game.puck);
            }

            if (freshMatch)
            {
                ClearRematchState();
                effects.bestRally = 0;
                effects.rally = 0;
                remotePaddle = game.hostPaddle;
            }

            effects.trail.clear();
            awaitingGoal = false;
            phaseAge = 0;
            lastCountdown = -1;
        }
        else if (messageID == MovePacketId)
        {
            MovePacket move;
            reader(move);
            if (move.side > 1)
            {
                return;
            }

            if (client.getRole() == Client::Role::Host
                && (move.side != 1 || sender != opponentPeerID))
            {
                return;
            }

            const Vec2 position = arena::clampPaddle(
                move.position,
                move.side
            );
            game.PaddlePosition(move.side) = position;

            // The Host is the relay point for the spectator view. Forward the
            // validated Client position so every spectator receives the same
            // target that the Host uses for its simulation.
            if (client.getRole() == Client::Role::Host)
            {
                (void)client.send(
                    MovePacketId,
                    Client::SendTarget::Others(),
                    MovePacket{ move.side, position }
                );
            }
        }
        else if (messageID == ImpactPacketId)
        {
            ImpactPacket impact;
            reader(impact);

            if (impact.match != game.match
                || impact.round != game.round
                || game.phase != Phase::Playing)
            {
                return;
            }

            game.puck =
                (arena::inside((impact.puck)));
            game.puckVelocity =
                (arena::limit(
                    (impact.velocity),
                    arena::MaxSpeed
                ));
            if (impact.side > 1)
            {
                return;
            }

            game.PaddlePosition(impact.side) =
                (arena::clampPaddle(
                    (impact.paddle),
                    impact.side
                ));
            awaitingGoal = false;
            effects.PaddleContact(
                game.puck,
                impact.side == 0 ? Red : Blue
            );
        }
        else if (messageID == GoalPacketId
            && side == 0
            && sender == opponentPeerID)
        {
            GoalPacket goal;
            reader(goal);

            const bool validGoal =
                goal.match == game.match
                && goal.round == game.round
                && game.phase == Phase::Playing
                && goal.position.x >= arena::GoalLeft + arena::Radius - 0.01
                && goal.position.x <= arena::GoalRight - arena::Radius + 0.01
                && goal.scorer == 0
                && goal.position.y <= 0.01;

            if (validGoal)
            {
                game.puck = goal.position;
                AwardGoal(goal.scorer);
            }
        }
    };
    }

    void update()
    {
        client.update();

        if (screen == Screen::Title)
        {
            if (MouseL.down())
            {
                ResetMatch(game);
                spectatorHostPaddle = game.hostPaddle;
                spectatorGuestPaddle = game.guestPaddle;
                phaseAge = 0;
                lastCountdown = -1;
                screen = Screen::Game;
            }

            return;
        }

        const double deltaTime = Min(Scene::DeltaTime(), 0.05);
        phaseAge += deltaTime;
        if (centerNoticeTimer > 0)
        {
            centerNoticeTimer = Max(0.0, centerNoticeTimer - deltaTime);
            if (centerNoticeTimer <= 0)
            {
                centerNotice.clear();
            }
        }

        roomListRefreshClock += deltaTime;
        const double roomListRefreshInterval = 5.0;
        const bool shouldRefreshRoomList = (screen == Screen::Lobby);
        if (shouldRefreshRoomList
            && !roomListLoading
            && roomListRefreshClock >= roomListRefreshInterval)
        {
            RefreshRoomList();
        }

        if (matchmaking
            && matchmakingRetryPending
            && roomListRefreshClock >= 1.0)
        {
            matchmakingRetryPending = false;
            StartMatchmakingRequest();
        }

        if (online && client.getState() == Client::ClientState::Disconnected)
        {
            online = false;
            memberReady = false;
            game.phase = Phase::Waiting;
        }

        const int side =
            (online && client.getRole() == Client::Role::Client) ? 1 : 0;
        const int viewSide = spectator ? 0 : side;
        const bool roomConnected =
            online
            && client.getState() == Client::ClientState::InRoom
            && !client.getMemberIDs().isEmpty();
        const bool linked =
            roomConnected
            && !spectator
            && (
                (client.getRole() == Client::Role::Host
                    && !opponentPeerID.isEmpty()
                    && client.getMemberIDs().contains(opponentPeerID))
                || (client.getRole() == Client::Role::Client
                    && client.getMemberIDs().contains(client.getHostPeerID()))
            );

        const bool cpuActive = !spectator && !linked;
        const int cpuSide =
            (online && client.getRole() == Client::Role::Client) ? 0 : 1;
        const bool authority = cpuActive || (online && side == 0);
        const bool active = !spectator && (cpuActive || linked);

        UpdateMenuInput(linked, active, authority);

        if (online && !spectator && memberReady && !linked)
        {
            game.phase = Phase::Waiting;
            memberReady = false;
        }

        const bool spectatorPlayback = spectator && roomConnected;

        const RectF rematchControl{
            Court.x + Court.w - 160,
            Court.y - 54,
            160,
            46
        };
        const RectF paddlePointerArea{
            Court.x,
            Court.y,
            Court.w,
            Scene::Height() - Court.y
        };

        if (MouseL.down()
            && screen == Screen::Game
            && paddlePointerArea.contains(Cursor::PosF())
            && !rematchControl.contains(Cursor::PosF()))
        {
            const Vec2 cursorWorld = ToWorldPosition(Cursor::PosF(), side);
            mousePaddleOffsetY =
                cursorWorld.y - game.PaddlePosition(side).y;
            mouseControl = true;
        }

        if (!MouseL.pressed())
        {
            mouseControl = false;
        }

        if (KeyM.down())
        {
            effects.muted = !effects.muted;
        }

        const Vec2 startPosition = game.PaddlePosition(side);
        Vec2 targetPosition = startPosition;
        const Vec2 opponentStartPosition = game.PaddlePosition(1 - side);

        if (!spectator && (practice || online) && game.phase != Phase::Finished)
        {
            const Vec2 keyboardInput{
                (KeyD.pressed() || KeyRight.pressed() ? 1.0 : 0.0)
                    - (KeyA.pressed() || KeyLeft.pressed() ? 1.0 : 0.0),
                (KeyS.pressed() || KeyDown.pressed() ? 1.0 : 0.0)
                    - (KeyW.pressed() || KeyUp.pressed() ? 1.0 : 0.0)
            };

            if (!keyboardInput.isZero())
            {
                targetPosition +=
                    keyboardInput.normalized() * 560 * deltaTime * (side == 1 ? -1 : 1);
            }

            if (mouseControl)
            {
                const Vec2 cursorWorld = ToWorldPosition(Cursor::PosF(), side);
                targetPosition = {
                    cursorWorld.x,
                    cursorWorld.y - mousePaddleOffsetY
                };
            }

            targetPosition =
                (arena::clampPaddle(
                    (targetPosition),
                    side
                ));
            game.PaddlePosition(side) = targetPosition;

            sendClock += deltaTime;

            if (linked
                && targetPosition.distanceFrom(lastSentPosition) > 0.05
                && sendClock >= 1.0 / 30)
            {
                const auto result = client.send(
                    MovePacketId,
                    Client::SendTarget::Others(),
                    MovePacket{
                        static_cast<uint8>(side),
                        targetPosition
                    }
                );

                if (result == Client::SendResult::Accepted)
                {
                    lastSentPosition = targetPosition;
                    sendClock = 0;
                }
            }
        }

        if (cpuActive)
        {
            // The CPU always controls the side opposite to the local player.
            // This matters while a client is waiting for the Host's channel:
            // the CPU must not take over the client's own paddle.
            const double homeY = (cpuSide == 0)
                ? (arena::Height - 85)
                : 85;
            Vec2 aim{ 250, homeY };

            const bool puckOnCpuSide = (cpuSide == 0)
                ? (game.puck.y > 295)
                : (game.puck.y < 345);
            if (game.phase == Phase::Playing && puckOnCpuSide)
            {
                // Stop short of the puck only when defending. For an attack,
                // move slightly into the contact distance so the paddle keeps
                // a forward velocity instead of parking directly behind it.
                const double strikeOffset =
                    arena::PaddleRadius + arena::Radius - 9.0;
                aim = {
                    game.puck.x,
                    game.puck.y + ((cpuSide == 0) ? strikeOffset : -strikeOffset)
                };

                const bool approachingCpu = (cpuSide == 0)
                    ? (game.puckVelocity.y > 0)
                    : (game.puckVelocity.y < 0);
                if (approachingCpu)
                {
                    aim.x += game.puckVelocity.x * ((cpuSide == 0) ? -0.13 : 0.13);
                }
            }

            aim = arena::clampPaddle(aim, cpuSide);

            // The CPU accelerates only when the puck is close enough to be a
            // real threat. Far from the puck it remains intentionally beatable.
            Vec2& cpuPaddle = game.PaddlePosition(cpuSide);
            const double puckDistance = cpuPaddle.distanceFrom(game.puck);
            const double urgency = 1.0 - Min(1.0, puckDistance / 240.0);
            const double cpuSpeed = 400.0 + 650.0 * urgency;

            cpuPaddle += arena::limit(
                aim - cpuPaddle,
                cpuSpeed * deltaTime
            );
        }

        if ((active || spectatorPlayback) && game.phase == Phase::Countdown)
        {
            game.timer = Max(0.0, game.timer - deltaTime);

            const int number = static_cast<int>(Ceil(game.timer));
            if (number != lastCountdown && number > 0 && !effects.muted)
            {
                effects.wall.playOneShot(0.25, 0, 1.8);
            }

            lastCountdown = number;

            if (game.timer <= 0)
            {
                game.phase = Phase::Playing;
                phaseAge = 0;
                idleClock = 0;

                if (authority)
                {
                    SendStateToOthers();
                }
            }
        }
        else if (active && game.phase == Phase::Goal)
        {
            game.timer = Max(0.0, game.timer - deltaTime);

            if (game.timer <= 0 && authority)
            {
                PrepareServe(game, 1 - game.scorer);
                effects.trail.clear();
                SendStateToOthers();
            }
        }
        else if ((active || spectatorPlayback)
            && game.phase == Phase::Playing
            && !awaitingGoal)
        {
            arena::Body puck{
                (game.puck),
                (game.puckVelocity)
            };

            const Vec2 physicsPaddleStart = spectator
                ? game.hostPaddle
                : startPosition;
            const Vec2 physicsPaddleEnd = spectator
                ? game.hostPaddle
                : targetPosition;
            const Vec2 physicsSecondStart = spectator
                ? game.guestPaddle
                : opponentStartPosition;
            const Vec2 physicsSecondEnd = spectator
                ? game.guestPaddle
                : game.PaddlePosition(1 - side);

            const auto result = arena::step(
                puck,
                (physicsPaddleStart),
                (physicsPaddleEnd),
                true,
                deltaTime,
                (physicsSecondStart),
                (physicsSecondEnd),
                spectator || cpuActive
            );

            if (result.paddleHit)
            {
                const int hitSide = spectator
                    ? result.paddleSide
                    : (result.paddleSide == 0 ? side : 1 - side);
                effects.PaddleContact(
                    result.contact,
                    hitSide == 0 ? Red : Blue
                );
            }

            if (result.wallHits)
            {
                effects.WallContact((result.contact));
            }

            game.puck = (puck.p);
            game.puckVelocity = (puck.v);

            if (result.paddleHit && linked)
            {
                (void)client.send(
                    ImpactPacketId,
                    Client::SendTarget::Others(),
                    ImpactPacket{
                        game.match,
                        game.round,
                        static_cast<uint8>(side),
                        game.puck,
                        game.puckVelocity,
                        targetPosition
                    }
                );
            }

            if (result.goal)
            {
                const bool localPlayerJudgesGoal =
                    !spectator
                    && (cpuActive
                        || (client.getRole() == Client::Role::Host
                            && result.goal == 2)
                        || (client.getRole() == Client::Role::Client
                            && result.goal == 1));

                if (localPlayerJudgesGoal)
                {
                    if (online && client.getRole() == Client::Role::Client)
                    {
                        (void)client.send(
                            GoalPacketId,
                            Client::SendTarget::Others(),
                            GoalPacket{
                                game.match,
                                game.round,
                                result.goal - 1,
                                game.puck
                            }
                        );
                    }

                    AwardGoal(result.goal - 1);
                }
                else
                {
                    awaitingGoal = true;
                }
            }

            idleClock = game.puckVelocity.length() < 4
                ? idleClock + deltaTime
                : 0;

            // Re-serve only after the puck has been stationary for a long time.
            if (idleClock > 30
                && authority)
            {
                PrepareServe(game, Random(0, 1));
                idleClock = 0;
                SendStateToOthers();
            }
        }

        if (spectator)
        {
            spectatorHostPaddle +=
                (game.hostPaddle - spectatorHostPaddle)
                * (1 - std::exp(-22 * deltaTime));
            spectatorGuestPaddle +=
                (game.guestPaddle - spectatorGuestPaddle)
                * (1 - std::exp(-22 * deltaTime));
        }
        else
        {
            remotePaddle +=
                (game.PaddlePosition(1 - side) - remotePaddle)
                * (1 - std::exp(-22 * deltaTime));
        }
        effects.Update(
            deltaTime,
            game.puck,
            viewSide,
            game.phase == Phase::Playing && !awaitingGoal
        );

        renderSide = side;
        renderViewSide = viewSide;
        renderCpuSide = cpuSide;
        renderLinked = linked;
        renderCpuActive = cpuActive;
        renderActive = active;
        renderAuthority = authority;
    }

    void draw()
    {
        if (screen == Screen::Title)
        {
            for (int x = 0; x < 720; x += 48)
            {
                Line{ x, 0, x, 1200 }.draw(
                    1,
                    ColorF{ 0.18, 0.49, 0.82, 0.045 }
                );
            }

            FontAsset(U"Display")(U"AIR HOCKEY")
                .drawAt(Vec2{ 360, 430 }, Text);
            FontAsset(U"Display")(U"ARENA")
                .drawAt(Vec2{ 360, 492 }, Blue);

            const RectF startButton{ 160, 610, 400, 82 };
            startButton.rounded(18).draw(Blue);
            FontAsset(U"Title")(U"タップで開始")
                .drawAt(startButton.center(), White);
            FontAsset(U"Small")(U"Tap anywhere to begin")
                .drawAt(Vec2{ 360, 748 }, Muted);
            return;
        }

        const int side = renderSide;
        const int viewSide = renderViewSide;
        const int cpuSide = renderCpuSide;
        const bool linked = renderLinked;
        const bool cpuActive = renderCpuActive;
        const bool active = renderActive;
        const bool authority = renderAuthority;

            // -----------------------------------------------------------------
            // Portrait UI
            // -----------------------------------------------------------------

        for (int x = 0; x < 720; x += 48)
        {
            Line{ x, 0, x, 1200 }.draw(1, ColorF{ 0.18, 0.49, 0.82, 0.045 });
        }

        if (screen == Screen::Lobby)
        {
            DrawIconButton({ 36, 25, 56, 56 }, U"\uf060");

            DrawButtonWithIcon(
                { 258, 25, 350, 56 },
                U"\uf067",
                U"部屋を作成",
                client.getState() == Client::ClientState::Disconnected,
                true
            );

            DrawPanel({ 36, 104, 576, 54 });
            FontAsset(U"Small")(U"公開中の部屋").draw(56, 122, Text);
            DrawRefreshButton(
                { 650, 131 },
                client.getState() == Client::ClientState::Disconnected
            );

            const auto& rooms = client.getRoomList();

            if (rooms.isEmpty() && !roomListLoading)
            {
                FontAsset(U"Title")(U"公開中の部屋はありません")
                    .drawAt(Vec2{ 360, 300 }, Muted);
                FontAsset(U"Small")(U"部屋を作成して待機できます")
                    .drawAt(Vec2{ 360, 340 }, Muted);
            }
            else if (!rooms.isEmpty())
            {
                const size_t visibleRoomCount = Min<size_t>(rooms.size(), 10);

                for (size_t index = 0; index < visibleRoomCount; ++index)
                {
                    const auto& roomInfo = rooms[index];
                    const double y = 180 + static_cast<double>(index) * 78;
                    const bool joinable =
                        roomInfo.isOpen
                        && roomInfo.participantCount < roomInfo.maxParticipants;
                    const bool isCurrentRoom =
                        roomInfo.id == client.getRoomID();
                    const bool canEnter = joinable && !isCurrentRoom;
                    const String roomAction = isCurrentRoom
                        ? U"参加中"
                        : (!roomInfo.isOpen
                            ? U"締切"
                            : (roomInfo.participantCount >= 2
                                ? U"観戦"
                                : U"参加"));

                    RectF row{ 36, y, 648, 64 };
                    const bool hovered = canEnter && row.mouseOver();
                    row.rounded(10).draw(
                        hovered
                            ? ColorF{ 0.90, 0.96, 1.0 }
                            : ColorF{ 0.985, 0.99, 1.0 }
                    );
                    row.rounded(10).drawFrame(
                        1,
                        ColorF{
                            isCurrentRoom ? Red : Blue,
                            canEnter || isCurrentRoom ? 0.42 : 0.16
                        }
                    );

                    String roomLabel = roomInfo.id;
                    if (roomLabel.size() > 24)
                    {
                        roomLabel = roomLabel.substr(0, 21) + U"…";
                    }

                    FontAsset(U"Body")(roomLabel).draw(56, y + 12, Text);
                    FontAsset(U"Small")(
                        U"{} / {}人"_fmt(
                            roomInfo.participantCount,
                            roomInfo.maxParticipants
                        )
                    ).draw(56, y + 38, Muted);
                    FontAsset(U"Small")(
                        roomAction
                    ).drawAt(
                        Vec2{ 630, y + 32 },
                        isCurrentRoom ? Red : (canEnter ? Blue : Muted)
                    );

                }
            }

        }
        else
        {
            const bool disconnected =
                client.getState() == Client::ClientState::Disconnected;
            const bool waitingForOpponent =
                matchmaking
                || (online
                    && client.getRole() == Client::Role::Host
                    && opponentPeerID.isEmpty());

            const String onlineLabel = spectator
                ? U"観戦中"
                : (waitingForOpponent
                    ? U"待機中…"
                    : (online && linked ? U"オンライン対戦中 (退出)" : U"オンライン対戦"));

            const bool fullscreen = IsSiv3DFullscreen();
            DrawIconButton(
                { 36, 24, 56, 56 },
                fullscreen ? U"\uf066" : U"\uf065"
            );

            DrawButton(
                { 104, 24, 472, 56 },
                onlineLabel,
                waitingForOpponent || disconnected || online,
                !online && !matchmaking
            );

            if (waitingForOpponent)
            {
                const Vec2 center{ 550, 52 };
                const double angle = Scene::Time() * 5;
                Circle{ center, 14 }.drawFrame(2, ColorF{ Blue, 0.25 });
                Line{
                    center,
                    center + Vec2{ Cos(angle), Sin(angle) } * 12
                }.draw(3, Blue);
            }

            DrawIconButton({ 586, 24, 98, 56 }, U"\uf03a");

            const int displaySide = viewSide;

            // remotePaddle is smoothed in the local world's coordinates. Pick
            // the smoothed value for the remote role, then render both roles
            // with their canonical colors from either player's viewpoint.
            const Vec2 displayedHostPaddle =
                spectator
                    ? spectatorHostPaddle
                    : (side == 0
                        ? game.hostPaddle
                        : (cpuActive ? game.hostPaddle : remotePaddle));
            const Vec2 displayedGuestPaddle =
                spectator
                    ? spectatorGuestPaddle
                    : (side == 1
                        ? game.guestPaddle
                        : (cpuActive ? game.guestPaddle : remotePaddle));

            FontAsset(U"Small")(U"FIRST TO 7").drawAt(Court.center().x, 106, Muted);
            FontAsset(U"Score")(game.hostScore)
                .drawAt(Court.center().x - 62, 136, Red);
            FontAsset(U"Small")(U"—").drawAt(Court.center().x, 136, Muted);
            FontAsset(U"Score")(game.guestScore)
                .drawAt(Court.center().x + 62, 136, Blue);

            DrawCourt(
                game,
                displaySide,
                displayedHostPaddle,
                displayedGuestPaddle,
                effects
            );

            if (cpuActive)
            {
                const Vec2 cpuPaddle = (cpuSide == 0)
                    ? displayedHostPaddle
                    : displayedGuestPaddle;
                DrawPaddleCaption(
                    ToViewPosition(cpuPaddle, displaySide),
                    U"CPU",
                    Blue
                );
            }

            String heading;
            if (centerNoticeTimer > 0)
            {
                heading = centerNotice;
            }
            else if (game.phase == Phase::Waiting)
            {
                heading = U"WAITING";
            }
            else if (game.phase == Phase::Countdown)
            {
                heading = game.timer > 0.05
                    ? Format(static_cast<int>(Ceil(game.timer)))
                    : U"READY";
            }
            else if (game.phase == Phase::Goal)
            {
                heading = game.scorer == displaySide ? U"NICE SHOT!" : U"GOAL";
            }
            else if (game.phase == Phase::Finished)
            {
                heading = game.Score(displaySide) >= WinningScore
                    ? U"VICTORY"
                    : U"GOOD GAME";
            }
            if (!heading.isEmpty())
            {
                RectF{
                    Court.x + 25,
                    Court.center().y - 73,
                    Court.w - 50,
                    146
                }.rounded(12).draw(ColorF{ White, 0.94 });
                if (centerNoticeTimer > 0)
                {
                    FontAsset(U"Title")(heading)
                        .drawAt(Court.center(), Text);
                }
                else
                {
                    FontAsset(U"Display")(heading)
                        .drawAt(Court.center(), Text);
                }
            }
            else if (phaseAge < 0.65)
            {
                FontAsset(U"Title")(U"PLAY")
                    .drawAt(Court.center(), ColorF{ Text, 1 - phaseAge / 0.65 });
            }

            const bool rematchAvailable = active
                && (online
                    ? linked
                    : authority);
            const bool onlineRematch = online && linked && !spectator;
            const String rematchLabel =
                onlineRematch
                    ? U"再試合 {}/2"_fmt(static_cast<int>(rematchReadyCount))
                    : U"再試合";
            const RectF rematchRect{
                Court.x + Court.w - 100,
                Court.y - 74,
                100,
                46
            };
            const ColorF selfRematchColor =
                (client.getRole() == Client::Role::Host) ? Red : Blue;
            const ColorF opponentRematchColor =
                (client.getRole() == Client::Role::Host) ? Blue : Red;
            const bool opponentRematchReady =
                onlineRematch && !rematchRequested && (rematchReadyCount > 0);
            if (onlineRematch)
            {
                DrawRematchButton(
                    rematchRect,
                    rematchLabel,
                    rematchAvailable,
                    rematchRequested,
                    opponentRematchReady,
                    selfRematchColor,
                    opponentRematchColor
                );
            }
            else
            {
                DrawButton(
                    rematchRect,
                    rematchLabel,
                    rematchAvailable,
                    false
                );
            }

            }
    }
};

void Main()
{
    Window::SetTitle(U"Air Hockey Arena");
    Window::SetStyle(WindowStyle::Sizable);
    Window::Resize(720, 1200);
    Scene::SetResizeMode(ResizeMode::Keep);
    Scene::SetLetterbox(Ink);
    Scene::SetTextureFilter(TextureFilter::Linear);
    Scene::SetBackground(Ink);

    FontAsset::Register(U"Display", 42, Typeface::Bold);
    FontAsset::Register(U"Score", 44, Typeface::Bold);
    FontAsset::Register(U"Title", 24, Typeface::Bold);
    FontAsset::Register(U"Body", 17);
    FontAsset::Register(U"Small", 14);
    FontAsset::Register(U"Micro", 12);
    FontAsset::Register(U"Icon", 24, Typeface::Icon_Awesome_Solid);

    MSRenderTexture gameRenderTexture{
        Size{ 720, 1200 },
        TextureFormat::R8G8B8A8_Unorm,
        HasDepth::No
    };

    AirHockeyGame game;

    while (System::Update())
    {
        game.update();

        gameRenderTexture.clear(Ink);
        {
            const ScopedRenderTarget2D renderTarget{ gameRenderTexture };
            game.draw();
        }

        Graphics2D::Flush();
        gameRenderTexture.resolve();
        gameRenderTexture.draw();
    }
}
