import SwiftUI

/// Pause menu driven identically by controller, keyboard, remote, mouse and
/// touch: the engine owns the selection, the view just reflects it.
struct PauseMenuView: View {
    let engine: GameEngine

    var body: some View {
        ZStack {
            Rectangle().fill(.black.opacity(0.45)).ignoresSafeArea()
                .onTapGesture { engine.menuBack() }
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    if engine.menuPage != .main {
                        Button { engine.menuBack() } label: { Image(systemName: "chevron.left") }
                            .buttonStyle(.plain)
                    }
                    Text(engine.menuTitle).font(.title.bold())
                    Spacer()
                }
                ScrollViewReader { proxy in
                    ScrollView {
                        VStack(spacing: 6) {
                            ForEach(Array(engine.menuItems.enumerated()), id: \.element.id) { index, item in
                                row(item, index: index, selected: index == engine.menuSelection)
                                    .id(item.id)
                            }
                        }
                    }
                    .scrollIndicators(.hidden)
                    .onChange(of: engine.menuSelection) { _, i in
                        let items = engine.menuItems
                        if items.indices.contains(i) { withAnimation { proxy.scrollTo(items[i].id, anchor: .center) } }
                    }
                }
                Text("Controle: direcional para navegar, ◀︎ ▶︎ ajusta, A confirma, B volta")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(28)
            .frame(maxWidth: 620, maxHeight: 640)
            .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 28))
            .padding(24)
        }
    }

    @ViewBuilder
    private func row(_ item: MenuItem, index: Int, selected: Bool) -> some View {
        HStack(spacing: 14) {
            if case .slot(let slot) = item.kind {
                Group {
                    if let thumb = slot.thumbnail {
                        Image(decorative: thumb, scale: 1).resizable().interpolation(.none)
                    } else {
                        Rectangle().fill(.white.opacity(0.08))
                    }
                }
                .frame(width: 96, height: 72)
                .clipShape(RoundedRectangle(cornerRadius: 8))
            }
            Text(item.title).font(.title3.weight(selected ? .semibold : .regular))
            Spacer()
            if let value = item.value {
                HStack(spacing: 6) {
                    if case .option = item.kind { Image(systemName: "chevron.left").font(.caption).opacity(selected ? 0.8 : 0) }
                    Text(value).foregroundStyle(.secondary).multilineTextAlignment(.trailing)
                    if case .option = item.kind { Image(systemName: "chevron.right").font(.caption).opacity(selected ? 0.8 : 0) }
                }
            }
            if case .submenu = item.kind { Image(systemName: "chevron.right").foregroundStyle(.secondary) }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
        .background(selected ? Color.white.opacity(0.16) : .clear, in: RoundedRectangle(cornerRadius: 14))
        .contentShape(Rectangle())
        .onTapGesture { engine.activate(index) }
    }
}
