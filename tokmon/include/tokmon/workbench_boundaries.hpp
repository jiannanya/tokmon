#pragma once

namespace white {
class NativeComponentRegistry;
}

namespace tokmon::desktop {

// Installs the product-owned native component boundaries referenced by
// workbench.html. The Arche workbench plugin owns their lifetime.
void register_workbench_boundaries(white::NativeComponentRegistry &registry);
void unregister_workbench_boundaries(white::NativeComponentRegistry &registry);

} // namespace tokmon::desktop
