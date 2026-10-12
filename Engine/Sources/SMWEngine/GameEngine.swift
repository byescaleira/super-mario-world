import CryptoKit
import Foundation
import GroupActivities
import MetalKit
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif
import Observation
import SMWCore

public struct MenuItem: Identifiable {
    public enum Kind { case action, submenu, option, slot(SaveSlot), info }
    public let id: String
    public let title: String
    public var value: String?
    public let kind: Kind
    var activate: (() -> Void)?
    var adjust: ((Int) -> Void)?
}

public enum MenuPage: Equatable { case main, save, load, settings, controls, online }

/// Owns the running game: frame loop, run-ahead, rewind, saves, audio, haptics
/// and the pause menu. All presentation and comfort features live here, around
/// the untouched game.
@MainActor
@Observable
public final class GameEngine {
    public enum Phase: Equatable { case loading, needsROM(String?), playing }

    public let settings = GameSettings()
    public let input = InputHub()
    public let gameCenter = GameCenterService()
    /// Short description of the online session, nil when playing alone.
    public private(set) var onlineStatus: String?
    public private(set) var phase: Phase = .loading
    public private(set) var isPaused = false
    public private(set) var rendererName = "—"
    public private(set) var menuPage: MenuPage = .main
    public var menuSelection = 0
    public private(set) var slots: [SaveSlot] = []
    public private(set) var toast: String?
    public private(set) var isRewinding = false
    public private(set) var isFastForwarding = false

    @ObservationIgnored private var core: EmulatorCore?
    @ObservationIgnored private let audio = AudioOutput()
    @ObservationIgnored private let haptics = Haptics()
    @ObservationIgnored private let saves = SaveStore()
    @ObservationIgnored private var renderer: FrameRenderer?
    @ObservationIgnored private var rewind: RewindBuffer?
    @ObservationIgnored private var runAheadState: [UInt8] = []
    @ObservationIgnored private var lastFrame = [UInt32](repeating: 0xFF00_0000, count: frameWords)
    @ObservationIgnored private var lastWidth = 256
    @ObservationIgnored private var output: DisplayOutput?
    @ObservationIgnored private var headroom: Float = 1
    /// 120 Hz presentation: the newest emulated frame waits here while its
    /// interpolated predecessor is on screen.
    @ObservationIgnored private let pending: UnsafeMutablePointer<UInt32> = {
        let p = UnsafeMutablePointer<UInt32>.allocate(capacity: frameWords)
        p.initialize(repeating: 0xFF00_0000, count: frameWords)
        return p
    }()
    @ObservationIgnored private var motionPhase = 0
    @ObservationIgnored private var highRefresh = false
    @ObservationIgnored private let vault = SaveVault()
    @ObservationIgnored private var netplay: NetplaySession?
    @ObservationIgnored private let matchmaker = Matchmaker()
    @ObservationIgnored private var romHash = Data()
    @ObservationIgnored private let groupState = GroupStateObserver()
    @ObservationIgnored private var sharePlayTask: Task<Void, Never>?
    @ObservationIgnored private var pendingRemoteSave: [UInt8]?
    @ObservationIgnored private var frameCounter = 0
    @ObservationIgnored private var toastTask: Task<Void, Never>?

    private static let frameWords = Int(SMW_MAX_FRAME_WIDTH) * 224
    private static let frameBytes = frameWords * 4
    private static let maxWideMargin = 112 // limit of tilemap data SMW keeps loaded

    private static let gameModeAddress = 0x0100 // SMWDisX rammap.asm: GameMode
    private static let gameModeLevel: UInt8 = 0x14
    private static let gameModeFileSelectLast: UInt8 = 0x0A // GameMode_PlayerSelect

    public init() {
        input.onControllerConnected = { [weak self] c in self?.haptics.controllerConnected(c) }
        input.onControllerDisconnected = { [weak self] c in self?.haptics.controllerDisconnected(c) }
        for c in input.controllers { haptics.controllerConnected(c) }
        input.wantsTouchControls = settings.showTouchControls
    }

    // MARK: Lifecycle

    public func boot() {
        guard core == nil else { return }
        do { try start(ROMLibrary.load()) } catch { phase = .needsROM(nil) }
    }

    public func importROM(at url: URL) {
        do { try start(ROMLibrary.importROM(from: url)) } catch {
            phase = .needsROM(error.localizedDescription)
        }
    }

    private func start(_ rom: Data) throws {
        guard let c = EmulatorCore(rom: rom) else { throw ROMError.invalid("Falha ao iniciar o runtime.") }
        core = c
        if let sram = vault.load() {
            c.loadSRAM(Data(sram))
        }
        vault.onRemoteChange = { [weak self] sram in self?.pendingRemoteSave = sram }
        if let resume = saves.loadResume() {
            if c.loadState(resume) { showToast("Continuando de onde você parou") }
        }
        c.setHDAudio(settings.hdAudio)
        romHash = Data(Insecure.SHA1.hash(data: rom))
        startOnlineServices()
        rewind = RewindBuffer(stateSize: EmulatorCore.stateSize)
        runAheadState = [UInt8](repeating: 0, count: EmulatorCore.stateSize)
        audio.volume = Float(settings.volume)
        audio.start()
        phase = .playing
    }

    /// Persist everything when the app leaves the foreground.
    public func suspend() {
        guard let core else { return }
        flushSRAM()
        saves.saveResume(core.saveState())
        audio.stop()
    }

    public func resume() {
        guard core != nil, !isPaused else { return }
        audio.start()
    }

    func attach(view: MTKView) {
        output = nil // a new view (e.g. after rotation) needs its format applied
        guard renderer == nil, let device = view.device else { return }
        do {
            let r = try RendererFactory.make(device: device)
            renderer = r
            rendererName = r.name
        } catch {
            print("Renderer error: \(error)")
        }
    }

    // MARK: Frame loop

    private func videoConfig(output: DisplayOutput) -> VideoConfig {
        let peak = output == .sdr ? 1 : min(Float(settings.hdrPeak), max(headroom, 1))
        return VideoConfig(filter: settings.filter, aspect: settings.aspect, crtColors: settings.crtColors,
                           curvature: settings.crtCurvature, scanlines: Float(settings.scanlineStrength),
                           output: output, hdrPeak: peak, wideGamut: settings.wideGamut)
    }

    /// Margin per side that makes the picture exactly fill the view.
    private func widescreenMargin(for view: MTKView) -> Int {
        guard settings.widescreen, let par = settings.aspect.pixelAspect else { return 0 }
        let size = view.drawableSize
        guard size.width > 0, size.height > 0 else { return 0 }
        let wanted = 224 * (size.width / size.height) / par
        return min(max(Int(((wanted - 256) / 2).rounded()), 0), Self.maxWideMargin)
    }

    private func updateDisplay(_ view: MTKView) -> DisplayOutput {
        let wanted = DisplayConfigurator.preferredOutput(hdr: settings.hdr)
        if wanted != output {
            DisplayConfigurator.apply(wanted, to: view)
            output = wanted
        }
        if frameCounter % 60 == 0 {
            headroom = DisplayConfigurator.headroom(of: view)
            let fps = settings.motion120 && DisplayConfigurator.maximumFramesPerSecond(of: view) >= 120 ? 120 : 60
            if view.preferredFramesPerSecond != fps { view.preferredFramesPerSecond = fps }
            if highRefresh != (fps == 120) {
                highRefresh = fps == 120
                core?.setMotionCapture(highRefresh)
                motionPhase = 0
            }
        }
        return wanted
    }

    /// Called once per display refresh by the Metal view.
    func tick(view: MTKView) {
        guard let renderer else { return }
        let output = updateDisplay(view)
        for event in input.menuEvents() { handleMenu(event) }
        let slot = renderer.acquireSlot()
        let fb = renderer.pixels(of: slot)
        if let core, phase == .playing, !isPaused {
            core.setWidescreen(widescreenMargin(for: view))
            if highRefresh {
                presentMotion(core, into: fb)
            } else {
                emulate(core, into: fb)
                frameCounter += 1
                lastWidth = core.frameWidth
                lastFrame.withUnsafeMutableBufferPointer { memcpy($0.baseAddress!, fb, Self.frameBytes) }
            }
        } else {
            frameCounter += 1
            lastFrame.withUnsafeBufferPointer { _ = memcpy(fb, $0.baseAddress!, Self.frameBytes) }
        }
        renderer.draw(slot: slot, activeWidth: lastWidth, in: view, config: videoConfig(output: output))
    }

    /// 120 Hz: even refreshes emulate the next 60 Hz game frame and show the
    /// moment halfway towards it; odd refreshes show the frame itself.
    private func presentMotion(_ core: EmulatorCore, into fb: UnsafeMutablePointer<UInt32>) {
        if motionPhase == 0 {
            emulate(core, into: pending)
            frameCounter += 1
            lastWidth = core.frameWidth
            let smooth = !isRewinding && !isFastForwarding && core.renderInterpolated(into: fb, t: 0.5)
            if !smooth { memcpy(fb, pending, Self.frameBytes) }
            lastFrame.withUnsafeMutableBufferPointer { _ = memcpy($0.baseAddress!, pending, Self.frameBytes) }
        } else {
            memcpy(fb, pending, Self.frameBytes)
        }
        motionPhase ^= 1
    }

    private func emulate(_ core: EmulatorCore, into fb: UnsafeMutablePointer<UInt32>) {
        if let netplay, netplay.phase != .ended("") {
            emulateOnline(netplay, core, into: fb)
            return
        }
        let keys = input.hotkeys()
        isRewinding = keys.rewind && settings.rewind
        isFastForwarding = keys.fastForward && !isRewinding
        let (p0, p1) = input.poll()
        core.setButtons(p0, port: 0)
        core.setButtons(p1, port: 1)

        if isRewinding, let rewind {
            if rewind.pop(into: core) {
                core.setFramebuffer(fb)
                core.runFrame()
                core.discardAudio()
            } else {
                lastFrame.withUnsafeBufferPointer { _ = memcpy(fb, $0.baseAddress!, Self.frameBytes) }
            }
            return
        }

        let steps = isFastForwarding ? 4 : 1
        for i in 0..<steps {
            step(core, display: i == steps - 1 ? fb : nil, keepAudio: !isFastForwarding)
        }
        if core.frameCount % 60 == 0 { applyRemoteSaveIfSafe(core) }
    }

    /// One real frame, optionally followed by run-ahead frames whose output is
    /// shown and then discarded: the screen reflects input 1-2 frames sooner
    /// than the game's own pipeline would, with identical game behavior.
    private func step(_ core: EmulatorCore, display: UnsafeMutablePointer<UInt32>?, keepAudio: Bool) {
        let ahead = display == nil ? 0 : settings.runAhead
        core.setFramebuffer(ahead == 0 ? display : nil)
        core.runFrame()
        if keepAudio { core.drainAudio(into: audio.ring) } else { core.discardAudio() }
        if core.takeSRAMDirty() { flushSRAM() }
        if core.frameCount % 60 == 0 { gameCenter.observe(wram: core.wram, player: 0) }
        if core.wram(Self.gameModeAddress) == Self.gameModeLevel {
            haptics.enabled = settings.haptics
            haptics.handle(events: core.soundEvents, useDevice: input.touchControlsVisible)
        }
        if settings.rewind, core.frameCount % 2 == 0 { rewind?.push(core) }

        guard ahead > 0 else { return }
        runAheadState.withUnsafeMutableBytes { core.saveState(into: $0.baseAddress!) }
        for i in 0..<ahead {
            core.setFramebuffer(i == ahead - 1 ? display : nil)
            core.runFrame()
        }
        runAheadState.withUnsafeBytes { _ = core.loadState(from: $0.baseAddress!) }
    }

    /// Persists the cartridge save. Cheap when nothing changed. An online
    /// guest is playing the host's save, which must not overwrite its own.
    private func flushSRAM() {
        guard let core, netplay == nil || netplay?.transport.isHost == true else { return }
        vault.store([UInt8](core.sram))
    }

    /// Progress synced from another device is only written into the running
    /// game while it is on the title / file select screens, where SMW reads
    /// the save files; mid-level it waits.
    private func applyRemoteSaveIfSafe(_ core: EmulatorCore) {
        guard let remote = pendingRemoteSave, core.wram(Self.gameModeAddress) <= Self.gameModeFileSelectLast else { return }
        core.loadSRAM(Data(remote))
        pendingRemoteSave = nil
        showToast("Save sincronizado do iCloud")
    }

    // MARK: Pause menu

    public func setPaused(_ paused: Bool) {
        guard phase == .playing else { return }
        guard paused != isPaused else {
            if !paused { menuPage = .main }
            return
        }
        isPaused = paused
        menuPage = .main
        menuSelection = 0
        gameCenter.setAccessPointVisible(paused)
        if paused {
            audio.stop()
            slots = saves.slots()
            flushSRAM()
        } else {
            audio.start()
        }
        input.touchButtons = 0
    }

    public func togglePause() { setPaused(!isPaused) }

    public var menuItems: [MenuItem] {
        switch menuPage {
        case .main: mainItems
        case .save, .load: slotItems
        case .settings: settingsItems
        case .controls: controlItems
        case .online: onlineItems
        }
    }

    public var menuTitle: String {
        switch menuPage {
        case .main: "Pausado"
        case .save: "Salvar estado"
        case .load: "Carregar estado"
        case .settings: "Configurações"
        case .controls: "Controles"
        case .online: "Multijogador online"
        }
    }

    public func activate(_ index: Int) {
        let items = menuItems
        guard items.indices.contains(index) else { return }
        menuSelection = index
        items[index].activate?()
    }

    public func adjust(_ index: Int, by delta: Int) {
        let items = menuItems
        guard items.indices.contains(index) else { return }
        items[index].adjust?(delta)
    }

    public func menuBack() {
        if menuPage == .main { setPaused(false) } else { open(.main) }
    }

    private func open(_ page: MenuPage) {
        if page == .save || page == .load { slots = saves.slots() }
        menuPage = page
        menuSelection = 0
    }

    private func handleMenu(_ event: MenuInput) {
        if event == .togglePause {
            togglePause()
            return
        }
        guard isPaused else { return }
        let count = menuItems.count
        switch event {
        case .up: menuSelection = (menuSelection - 1 + count) % max(count, 1)
        case .down: menuSelection = (menuSelection + 1) % max(count, 1)
        case .left: adjust(menuSelection, by: -1)
        case .right: adjust(menuSelection, by: 1)
        case .confirm: activate(menuSelection)
        case .back: menuBack()
        case .togglePause: break
        }
    }

    private var mainItems: [MenuItem] {
        [
            MenuItem(id: "resume", title: "Continuar", kind: .action, activate: { [weak self] in self?.setPaused(false) }),
            MenuItem(id: "save", title: "Salvar estado", kind: .submenu, activate: { [weak self] in self?.open(.save) }),
            MenuItem(id: "load", title: "Carregar estado", kind: .submenu, activate: { [weak self] in self?.open(.load) }),
            MenuItem(id: "online", title: "Multijogador online", value: onlineStatus, kind: .submenu,
                     activate: { [weak self] in self?.open(.online) }),
            MenuItem(id: "settings", title: "Configurações", kind: .submenu, activate: { [weak self] in self?.open(.settings) }),
            MenuItem(id: "controls", title: "Controles", kind: .submenu, activate: { [weak self] in self?.open(.controls) }),
            MenuItem(id: "reset", title: "Reiniciar jogo", kind: .action, activate: { [weak self] in self?.resetGame() }),
        ]
    }

    private var slotItems: [MenuItem] {
        slots.map { slot in
            let value = slot.date.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? "Vazio"
            return MenuItem(id: "slot\(slot.id)", title: "Slot \(slot.id)", value: value, kind: .slot(slot),
                            activate: { [weak self] in self?.useSlot(slot.id) })
        }
    }

    /// Keyboard-shortcut save/load (macOS menu commands).
    public func quickSave(slot: Int) {
        menuPage = .save
        useSlot(slot)
    }

    public func quickLoad(slot: Int) {
        menuPage = .load
        useSlot(slot)
    }

    public func reset() { resetGame() }

    private func useSlot(_ slot: Int) {
        guard let core else { return }
        if netplay != nil, menuPage == .load {
            showToast("Indisponível durante uma partida online")
            return
        }
        if menuPage == .save {
            let thumb = lastFrame.withUnsafeBufferPointer {
                SaveStore.image(fromFrame: $0.baseAddress!, width: lastWidth, pitch: Int(SMW_MAX_FRAME_WIDTH))
            }
            saves.saveState(core.saveState(), slot: slot, thumbnail: thumb)
            showToast("Estado salvo no slot \(slot)")
            setPaused(false)
        } else if let data = saves.loadState(slot: slot), core.loadState(data) {
            rewind?.reset()
            showToast("Slot \(slot) carregado")
            setPaused(false)
        } else if !(slots.first { $0.id == slot }?.isEmpty ?? true) {
            showToast("Estado de uma versão anterior do app — use o save do jogo")
        }
    }

    private func resetGame() {
        if netplay != nil {
            showToast("Indisponível durante uma partida online")
            return
        }
        core?.reset(hard: false)
        rewind?.reset()
        saves.clearResume()
        setPaused(false)
    }

    private func cycle<T: CaseIterable & Equatable>(_ value: T, _ delta: Int) -> T {
        let all = Array(T.allCases)
        let i = all.firstIndex(of: value) ?? 0
        return all[(i + delta + all.count) % all.count]
    }

    private func toggleItem(_ id: String, _ title: String, _ value: Bool, _ set: @escaping (Bool) -> Void) -> MenuItem {
        MenuItem(id: id, title: title, value: value ? "Sim" : "Não", kind: .option,
                 activate: { set(!value) }, adjust: { _ in set(!value) })
    }

    private var settingsItems: [MenuItem] {
        let s = settings
        var items: [MenuItem] = [
            MenuItem(id: "filter", title: "Filtro de vídeo", value: s.filter.title, kind: .option,
                     activate: { [weak self] in self.map { s.filter = $0.cycle(s.filter, 1) } },
                     adjust: { [weak self] d in self.map { s.filter = $0.cycle(s.filter, d) } }),
            MenuItem(id: "aspect", title: "Proporção", value: s.aspect.title, kind: .option,
                     activate: { [weak self] in self.map { s.aspect = $0.cycle(s.aspect, 1) } },
                     adjust: { [weak self] d in self.map { s.aspect = $0.cycle(s.aspect, d) } }),
        ]
        items.append(toggleItem("wide", "Widescreen (preenche a tela)", s.widescreen) { s.widescreen = $0 })
        items.append(toggleItem("hdr", "HDR / Dolby Vision", s.hdr) { s.hdr = $0 })
        if s.hdr {
            items.append(MenuItem(id: "hdrPeak", title: "Brilho HDR dos realces", value: String(format: "%.1f×", s.hdrPeak),
                                  kind: .option,
                                  activate: { s.hdrPeak = s.hdrPeak >= 4 ? 1 : s.hdrPeak + 0.5 },
                                  adjust: { d in s.hdrPeak = min(4, max(1, s.hdrPeak + Double(d) * 0.5)) }))
            items.append(toggleItem("gamut", "Cores amplas (P3 vívido)", s.wideGamut) { s.wideGamut = $0 })
        }
        if s.filter == .crt {
            items.append(MenuItem(id: "scan", title: "Scanlines", value: "\(Int(s.scanlineStrength * 100))%", kind: .option,
                                  activate: { s.scanlineStrength = s.scanlineStrength >= 1 ? 0 : min(1, s.scanlineStrength + 0.1) },
                                  adjust: { d in s.scanlineStrength = min(1, max(0, s.scanlineStrength + Double(d) * 0.1)) }))
            items.append(toggleItem("curve", "Curvatura da tela", s.crtCurvature) { s.crtCurvature = $0 })
        }
        items += [
            toggleItem("crtColors", "Cores de TV CRT", s.crtColors) { s.crtColors = $0 },
            toggleItem("hdAudio", "Áudio HD (interpolação cúbica)", s.hdAudio) { [weak self] v in
                s.hdAudio = v
                self?.core?.setHDAudio(v)
            },
            MenuItem(id: "volume", title: "Volume", value: "\(Int((s.volume * 100).rounded()))%", kind: .option,
                     activate: { [weak self] in
                         s.volume = s.volume >= 1 ? 0 : min(1, s.volume + 0.1)
                         self?.audio.volume = Float(s.volume)
                     },
                     adjust: { [weak self] d in
                         s.volume = min(1, max(0, s.volume + Double(d) * 0.1))
                         self?.audio.volume = Float(s.volume)
                     }),
            MenuItem(id: "runahead", title: "Redução de latência", value: s.runAhead == 0 ? "Desligada" : "\(s.runAhead) frame\(s.runAhead > 1 ? "s" : "")",
                     kind: .option,
                     activate: { s.runAhead = (s.runAhead + 1) % 3 },
                     adjust: { d in s.runAhead = min(2, max(0, s.runAhead + d)) }),
            toggleItem("rewind", "Rebobinar (gatilho esquerdo)", s.rewind) { [weak self] v in
                s.rewind = v
                if !v { self?.rewind?.reset() }
            },
            toggleItem("haptics", "Vibração", s.haptics) { s.haptics = $0 },
        ]
        #if !os(tvOS)
        items.append(toggleItem("motion", "Movimento 120 Hz (ProMotion)", s.motion120) { s.motion120 = $0 })
        #endif
        #if os(iOS)
        items.append(toggleItem("touch", "Controles na tela", s.showTouchControls) { [weak self] v in
            s.showTouchControls = v
            self?.input.wantsTouchControls = v
        })
        #endif
        items.append(MenuItem(id: "renderer", title: "Renderizador", value: rendererName, kind: .info))
        return items
    }

    private var controlItems: [MenuItem] {
        let rows: [(String, String)] = [
            ("Pular", "A (Xbox) · ✕ (PS) · B (Switch) · Z"),
            ("Pulo giratório", "B · ○ · A · X"),
            ("Correr / pegar", "X · □ · Y · A"),
            ("L / R (câmera)", "LB / RB · L1 / R1 · Q / W"),
            ("Start / Select", "Menu / Options · Enter / Tab"),
            ("Rebobinar", "Gatilho esquerdo · Backspace"),
            ("Avanço rápido", "Gatilho direito · `"),
            ("Pausar", "PS / Home · touchpad · Start+Select · Esc"),
        ]
        return rows.map { MenuItem(id: $0.0, title: $0.0, value: $0.1, kind: .info) }
    }

    // MARK: Online play (Game Center + SharePlay)

    private func startOnlineServices() {
        gameCenter.authenticate()
        gameCenter.onInviteAccepted = { [weak self] invite in
            self?.matchmaker.findMatch(invite: invite) { match in
                if let match { self?.beginOnline(GameCenterTransport(match: match)) }
            }
        }
        sharePlayTask = Task { [weak self] in
            for await session in PlayTogetherActivity.sessions() {
                self?.joinSharePlay(session)
            }
        }
    }

    private func joinSharePlay(_ session: GroupSession<PlayTogetherActivity>) {
        session.join()
        Task { [weak self] in
            // Roles are decided once both devices are in the session.
            for await participants in session.$activeParticipants.values where participants.count >= 2 {
                self?.beginOnline(SharePlayTransport(session: session))
                return
            }
        }
    }

    private func beginOnline(_ transport: NetplayTransport) {
        guard let core else { return }
        netplay?.end("Nova sessão")
        rewind?.reset()
        let session = NetplaySession(transport: transport, core: core, romHash: romHash)
        session.onAudio = { [weak self] core in
            guard let self else { return }
            core.drainAudio(into: self.audio.ring)
        }
        session.onPhaseChange = { [weak self] phase in self?.onlinePhaseChanged(phase) }
        netplay = session
        let role = transport.isHost ? "Mario" : "Luigi"
        onlineStatus = "\(transport.name) · \(transport.peerName)"
        showToast(transport.isHost ? "Online como Mario — escolha 2 PLAYER GAME" : "Online como \(role)")
        setPaused(false)
    }

    private func onlinePhaseChanged(_ phase: NetplaySession.Phase) {
        if case .ended(let reason) = phase {
            netplay = nil
            onlineStatus = nil
            rewind?.reset()
            showToast(reason)
        }
    }

    private func emulateOnline(_ session: NetplaySession, _ core: EmulatorCore, into fb: UnsafeMutablePointer<UInt32>) {
        isRewinding = false
        isFastForwarding = false
        let (p0, p1) = input.poll()
        if session.advance(localButtons: p0 | p1, display: fb) {
            if core.takeSRAMDirty() { flushSRAM() }
            if core.frameCount % 60 == 0 { gameCenter.observe(wram: core.wram, player: session.localPort) }
            haptics.enabled = settings.haptics
            if core.wram(Self.gameModeAddress) == Self.gameModeLevel {
                haptics.handle(events: core.soundEvents, useDevice: input.touchControlsVisible)
            }
            onlineStatus = "\(session.transport.name) · \(session.transport.peerName) · \(session.lag) quadros"
        } else {
            lastFrame.withUnsafeBufferPointer { _ = memcpy(fb, $0.baseAddress!, Self.frameBytes) }
        }
    }

    private func startSharePlay() {
        #if os(tvOS)
        showToast("No Apple TV, inicie o SharePlay pelo FaceTime no iPhone")
        #else
        let activity = PlayTogetherActivity()
        if groupState.isEligibleForGroupSession {
            Task {
                if case .activationPreferred = await activity.prepareForActivation() {
                    _ = try? await activity.activate()
                }
            }
        } else if let controller = try? GroupActivitySharingController(activity) {
            Presenter.present(controller)
        }
        #endif
    }

    private var onlineItems: [MenuItem] {
        var items: [MenuItem] = [
            MenuItem(id: "status", title: netplay == nil ? "Desconectado" : "Conectado",
                     value: onlineStatus ?? (gameCenter.playerName.map { "Game Center: \($0)" } ?? "Game Center desconectado"),
                     kind: .info),
            MenuItem(id: "shareplay", title: "Jogar com amigo via SharePlay", kind: .action,
                     activate: { [weak self] in self?.startSharePlay() }),
            MenuItem(id: "gcmatch", title: "Partida no Game Center", kind: .action, activate: { [weak self] in
                self?.matchmaker.findMatch { match in
                    if let match { self?.beginOnline(GameCenterTransport(match: match)) }
                }
            }),
            MenuItem(id: "gcdash", title: "Conquistas e placar", kind: .action,
                     activate: { [weak self] in self?.gameCenter.showDashboard() }),
        ]
        if netplay != nil {
            items.append(MenuItem(id: "leave", title: "Sair da partida online", kind: .action,
                                  activate: { [weak self] in self?.netplay?.end("Você saiu da partida") }))
        }
        return items
    }

    private func showToast(_ text: String) {
        toast = text
        toastTask?.cancel()
        toastTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(2.5))
            guard !Task.isCancelled else { return }
            self?.toast = nil
        }
    }
}
