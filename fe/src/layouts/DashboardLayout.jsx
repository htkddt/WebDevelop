import React from 'react';

export default function DashboardLayout({ children }) {
    return (
        <div className="flex h-screen overflow-hidden bg-gray-100">
            {/* Sidebar - Dùng màu darkBlue đã config */}
            <aside className="w-64 bg-gray-900 text-white flex-shrink-0 flex flex-col">
                <div className="p-6 flex items-center space-x-3">
                    <div className="w-8 h-8 bg-blue-500 rounded"></div>
                    <span className="font-bold text-lg">HRConnect HRM</span>
                </div>

                <nav className="flex-1 px-4 space-y-2">
                    {['Dashboard', 'Employees', 'Payroll', 'Leave'].map(item => (
                        <div key={item} className="p-3 text-gray-400 hover:bg-gray-800 rounded-lg cursor-pointer">{item}</div>
                    ))}
                    <div className="p-3 bg-white bg-opacity-10 text-white rounded-lg cursor-pointer font-medium">Permissions</div>
                </nav>

                <div className="p-6 border-t border-gray-800">
                    <button className="text-gray-400 hover:text-white">Logout</button>
                </div>
            </aside>

            {/* Main Content Area */}
            <div className="flex-1 flex flex-col overflow-hidden">
                <header className="h-16 bg-white border-b flex items-center justify-between px-8">
                    <h2 className="text-gray-800 font-semibold text-lg">HRConnect</h2>
                    <div className="flex items-center space-x-4">
                        <div className="text-right">
                            <p className="text-sm font-bold">Sarah Jenkins</p>
                            <p className="text-xs text-gray-500">HR Admin</p>
                        </div>
                        <img src="https://i.pravatar.cc/40" className="rounded-full w-10 h-10" alt="admin" />
                    </div>
                </header>

                <main className="flex-1 overflow-y-auto p-8">
                    {children}
                </main>
            </div>
        </div>
    );
}