const RoleRow = ({ role, description, users, status, selected }) => (
    <tr className="border-b last:border-0 hover:bg-gray-50">
        <td className="p-4 text-center"><input type="checkbox" checked={selected} readOnly /></td>
        <td className="p-4 font-medium">{role}</td>
        <td className="p-4 text-gray-500">{description}</td>
        <td className="p-4 text-center">{users}</td>
        <td className="p-4 text-center">
            <span className={`px-2 py-1 rounded text-xs ${status === 'Active' ? 'bg-green-100 text-green-600' : 'bg-gray-100 text-gray-500'}`}>
                {status}
            </span>
        </td>
        <td className="p-4 text-right space-x-2">
            <button className="text-gray-600 hover:underline text-sm">View</button>
            <button className="text-blue-600 hover:underline text-sm">Edit</button>
            <button className="text-red-500 hover:underline text-sm">Delete</button>
        </td>
    </tr>
);

export default function PermissionTable() {
    const roles = [
        { name: 'Administrator', desc: 'Administrator for management', users: 5, status: 'Active', selected: true },
        { name: 'HR Manager', desc: 'HR Manager for department employees', users: 3, status: 'Active' },
        { name: 'Department Head', desc: 'Department Head management', users: 2, status: 'Inactive' },
    ];

    return (
        <div className="bg-white rounded-xl shadow-sm border overflow-hidden">
            <table className="w-full text-left border-collapse">
                <thead className="bg-gray-50 text-gray-600 text-sm uppercase">
                    <tr>
                        <th className="p-4 w-10"></th>
                        <th className="p-4">Role Name</th>
                        <th className="p-4">Description</th>
                        <th className="p-4 text-center">Assigned Users</th>
                        <th className="p-4 text-center">Status</th>
                        <th className="p-4 text-right">Actions</th>
                    </tr>
                </thead>
                <tbody>
                    {roles.map((r, i) => <RoleRow key={i} role={r.name} description={r.desc} users={r.users} status={r.status} selected={r.selected} />)}
                </tbody>
            </table>
        </div>
    );
}