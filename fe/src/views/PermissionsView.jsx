import DashboardLayout from '../layouts/DashboardLayout';
import PermissionTable from '../components/permissions/PermissionTable';

export default function PermissionsView() {
    return (
        <DashboardLayout>
            <div className="max-w-6xl mx-auto">
                <h3 className="text-2xl font-bold mb-6">Permissions Management</h3>

                {/* Top Actions */}
                <div className="flex justify-between items-center mb-6">
                    <div className="flex gap-2">
                        <button className="bg-blue-600 text-white px-4 py-2 rounded-lg flex items-center gap-2 hover:bg-blue-700 transition">
                            <span>+</span> Create New Role
                        </button>
                        <button className="bg-gray-500 text-white px-4 py-2 rounded-lg flex items-center gap-2 hover:bg-gray-600 transition">
                            <span>👤</span> Assign Role
                        </button>
                    </div>
                    <div className="relative">
                        <input type="text" placeholder="Search Bar" className="border rounded-lg px-4 py-2 w-64 outline-none focus:ring-2 focus:ring-blue-500" />
                    </div>
                </div>

                <PermissionTable />

                {/* Bottom Details Section */}
                <div className="grid grid-cols-3 gap-6 mt-8">
                    <div className="col-span-2 bg-white p-6 rounded-xl border shadow-sm">
                        <h4 className="font-bold border-b pb-4 mb-4">Administrator Permissions</h4>
                        <div className="space-y-4">
                            {[
                                { label: 'Employee Mgmt', rights: ['Create', 'View', 'Edit', 'Delete'] },
                                { label: 'Payroll Mgmt', rights: ['View', 'Edit'] },
                                { label: 'Leave Mgmt', rights: ['Approve'] },
                            ].map((item, idx) => (
                                <div key={idx} className="flex items-center justify-between py-1">
                                    <span className="font-medium text-gray-700 w-32">{item.label}</span>
                                    <div className="flex-1 flex gap-4">
                                        {item.rights.map(r => (
                                            <label key={r} className="flex items-center gap-2 text-sm text-gray-600">
                                                <input type="checkbox" checked readOnly className="accent-blue-600" /> {r}
                                            </label>
                                        ))}
                                    </div>
                                </div>
                            ))}
                        </div>
                    </div>

                    <div className="bg-white p-6 rounded-xl border shadow-sm flex flex-col justify-center items-center text-center">
                        <div className="text-gray-500 text-lg">Total Roles: <span className="font-bold text-black">6</span></div>
                        <div className="text-gray-500 mt-2">
                            Admin Users: <span className="font-bold text-black">3</span> | Staff Users: <span className="font-bold text-black">84</span>
                        </div>
                    </div>
                </div>
            </div>
        </DashboardLayout>
    );
}