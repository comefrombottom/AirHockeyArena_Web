# pragma once

# include <Siv3D.hpp>
# include <functional>
# include <memory>

namespace s3d
{
	/// @brief WebRTC P2P クライアント
	///
	/// Native 版ではネイティブ WebRTC を使用し、Web 版では JavaScript の
	/// WebRTC 実装を使用します。両環境でこの公開 API は共通です。
	class WebRTCP2PClient
	{
	public:
		using PeerID = String;
		using RoomID = String;
		using MessageID = uint32;
		using Payload = Array<uint8>;
		using RoomPropertyTable = HashTable<uint8, String>;

		struct IceServer
		{
			String url;
			String username;
			String credential;
		};

		struct ClientOptions
		{
			String signalingURL;
			/// @brief Roomを分離するアプリケーションID。同じIDのクライアントだけが同じRoom空間を共有します。
			String applicationId = U"default";
			String peerID;
			Array<IceServer> iceServers;
			bool autoReconnect = true;
		};

		struct RoomCreationInfo
		{
			bool listed = true;
			bool isOpen = true;
			int32 maxParticipants = 8;
			RoomPropertyTable properties;
		};

		struct JoinRandomRoomOptions
		{
			RoomPropertyTable requiredRoomProperties;
			Optional<int32> expectedParticipantCount;
			Optional<int32> expectedMaxParticipants;
		};

		struct RoomInfo
		{
			RoomID id;
			bool listed = true;
			bool isOpen = true;
			int32 participantCount = 0;
			int32 maxParticipants = 8;
			RoomPropertyTable properties;
		};

		enum class ClientState : uint8
		{
			Disconnected,
			JoiningRoom,
			InRoom,
			LeavingRoom,
			RejoiningRoom,
		};

		enum class Role : uint8
		{
			None,
			Host,
			Client,
		};

		enum class ReconnectReason : uint8
		{
			SignalingLost,
			NetworkChanged,
			PeerConnectionLost,
			Unknown,
		};

		enum class MemberLeaveReason : uint8
		{
			Left,
			Disconnected,
			Replaced,
			RoomClosed,
			Unknown,
		};

		enum class RoomErrorCode : uint8
		{
			Unknown,
			InvalidState,
			RoomNotFound,
			RoomAlreadyExists,
			RoomClosed,
			RoomFull,
			InvalidRoomInfo,
			SignalingError,
			TransportError,
		};

		struct RoomError
		{
			RoomErrorCode code = RoomErrorCode::Unknown;
			String message;
		};

		enum class SendResult : uint8
		{
			Accepted,
			NotInRoom,
			NoRoute,
			InvalidTarget,
			MessageTooLarge,
		};

		class SendTarget
		{
		public:
			static SendTarget Others();
			static SendTarget All();
			static SendTarget Peers(const Array<PeerID>& peerIDs);

			[[nodiscard]] bool isOthers() const noexcept;
			[[nodiscard]] bool isAll() const noexcept;
			[[nodiscard]] bool isPeerList() const noexcept;
			[[nodiscard]] const Array<PeerID>& peerIDs() const noexcept;

		private:
			enum class Type : uint8
			{
				Others,
				All,
				PeerList,
			};

			SendTarget(Type type, Array<PeerID> peerIDs);

			Type m_type = Type::Others;
			Array<PeerID> m_peerIDs;
		};

		/// @brief クライアントを作成します。シグナリング接続はRoom操作時に開始します。
		WebRTCP2PClient();
		explicit WebRTCP2PClient(const ClientOptions& options);

		~WebRTCP2PClient();

		WebRTCP2PClient(const WebRTCP2PClient&) = delete;
		WebRTCP2PClient& operator=(const WebRTCP2PClient&) = delete;

		/// @brief 通信イベントをゲームのメインスレッドで処理します。
		void update();

		[[nodiscard]] ClientState getState() const noexcept;
		[[nodiscard]] Role getRole() const noexcept;
		[[nodiscard]] const PeerID& getPeerID() const noexcept;
		[[nodiscard]] const RoomID& getRoomID() const noexcept;
		[[nodiscard]] const PeerID& getHostPeerID() const noexcept;
		/// @brief DataChannel が開通済みのRoomメンバーを取得します。
		[[nodiscard]] Array<PeerID> getMemberIDs() const;
		[[nodiscard]] const RoomInfo& getRoomInfo() const noexcept;
		[[nodiscard]] const Array<RoomInfo>& getRoomList() const noexcept;

		/// @brief 衝突しにくいランダムなRoom IDを生成します。
		[[nodiscard]] static RoomID GenerateRandomRoomId();

		/// @brief 新しいRoomを作成して入室します。
		bool createRoom(const RoomID& roomID);
		bool createRoom(const RoomID& roomID, const RoomCreationInfo& info);

		/// @brief 既存のRoomに入室します。
		bool joinRoom(const RoomID& roomID);

		/// @brief Roomがあれば入室し、なければ作成して入室します。
		bool joinOrCreateRoom(const RoomID& roomID);
		bool joinOrCreateRoom(const RoomID& roomID, const RoomCreationInfo& info);

		/// @brief 条件に一致する公開Roomへランダムに入室します。
		bool joinRandom(const JoinRandomRoomOptions& options = {});

		/// @brief 条件に一致するRoomへ入室し、なければ指定情報でRoomを作成します。
		bool joinRandomOrCreateRoom(
			const JoinRandomRoomOptions& options,
			const RoomID& createRoomID,
			const RoomCreationInfo& info
		);

		/// @brief 明示的にRoomを退出します。自動再接続も停止します。
		void leaveRoom();

		/// @brief 公開Room一覧を更新します。
		bool refreshRoomList();

		/// @brief Room内の新規参加受付を変更します。
		bool setRoomOpen(bool isOpen);

		/// @brief Room設定を変更します。
		bool setRoomProperty(uint8 key, StringView value);

		[[nodiscard]] String getRoomProperty(uint8 key) const;
		[[nodiscard]] const RoomPropertyTable& getRoomProperties() const noexcept;

		/// @brief シリアライズ済みデータを送信します。
		[[nodiscard]] SendResult send(MessageID messageID, const Payload& payload, const SendTarget& target = SendTarget::Others());

		/// @brief MemoryWriterでシリアライズ済みのデータを送信します。
		[[nodiscard]] SendResult send(MessageID messageID, const Serializer<MemoryWriter>& writer, const SendTarget& target = SendTarget::Others());

		/// @brief Siv3DのSerializerで引数をシリアライズして送信します。
		template <class... Args>
		[[nodiscard]] SendResult send(MessageID messageID, const SendTarget& target, const Args&... args)
		{
			return send(messageID, Serializer<MemoryWriter>{}(args...), target);
		}

		// Room / connection events
		std::function<void(ClientState)> onStateChanged;
		std::function<void(const RoomInfo&)> onJoinedRoom;
		std::function<void(ReconnectReason)> onReconnectingRoom;
		std::function<void()> onReconnectedRoom;
		std::function<void(const RoomError&)> onRoomError;

		// Member events are notified only after the member's DataChannel is ready to send.
		std::function<void(const PeerID&)> onMemberJoined;
		std::function<void(const PeerID&)> onMemberReconnected;
		std::function<void(const PeerID&, MemberLeaveReason)> onMemberLeft;

		std::function<void(Role, const PeerID&)> onRoleChanged;

		std::function<void(const RoomInfo&)> onRoomInfoChanged;
		std::function<void(uint8, StringView)> onRoomPropertyChanged;
		std::function<void(bool)> onRoomOpenChanged;
		std::function<void()> onRoomListUpdated;

		std::function<void(const PeerID&, MessageID, Deserializer<MemoryViewReader>&)> onMessage;

	private:
		class Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
